#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "mcp_server.h"
#include "ogg_demuxer.h"
#include "button.h"
#include "config.h"

#include <esp_log.h>
#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_timer.h>
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_video.h"
#include "led/circular_strip.h"
#include "esp_lcd_jd9853.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "esp_audio_types.h"
#include <atomic>
#include <cstring>

#define TAG "waveshare_s3_audio_board"

#define LCD_OPCODE_WRITE_CMD        (0x02ULL)
#define LCD_OPCODE_READ_CMD         (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR      (0x32ULL)

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander = NULL;
    LcdDisplay* display_;
    EspVideo* camera_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_IO,
            .scl_io_num = I2C_SCL_IO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
    
    void InitializeTca9555(void)
    {
        esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus_, I2C_ADDRESS, &io_expander);  
        if(ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");                                                                                  // 打印引脚状态

        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_8|IO_EXPANDER_PIN_NUM_5|IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);                 // 设置引脚 EXIO0 和 EXIO1 模式为输出
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);                                // 复位 LCD 与 TouchPad
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(10));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);                                // 复位 LCD 与 TouchPad
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(10));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);                                // 复位 LCD 与 TouchPad
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_8, 1);                                                         // 启用喇叭功放
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_5, false);                                                     // 复位摄像头
        vTaskDelay(pdMS_TO_TICKS(5));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, true); 
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_ERROR_CHECK(ret);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeJd9853Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片JD9853
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        //ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 34));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        McpServer::GetInstance().AddTool(
            "self.music.play",
            "Play audio on the device speaker from a URL. "
            "The URL must point to an OGG/Opus audio stream. "
            "Always call this tool immediately after obtaining a music URL.",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("title", kPropertyTypeString),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string url = properties["url"].value<std::string>();
                std::string title = properties["title"].value<std::string>();

                struct Params { std::string url; std::string title; };
                auto* p = new Params{url, title};

                xTaskCreate([](void* arg) {
                    auto* p = static_cast<Params*>(arg);
                    std::string url = std::move(p->url);
                    std::string title = std::move(p->title);
                    delete p;

                    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                    if (!http->Open("GET", url)) {
                        ESP_LOGE(TAG, "music.play: open failed: %s", url.c_str());
                        vTaskDelete(nullptr);
                        return;
                    }
                    if (http->GetStatusCode() != 200) {
                        ESP_LOGE(TAG, "music.play: HTTP %d", http->GetStatusCode());
                        http->Close();
                        vTaskDelete(nullptr);
                        return;
                    }

                    ESP_LOGI(TAG, "music.play: streaming '%s'", title.c_str());

                    // PCM packet queue between decoder and output
                    struct PcmPacket { std::vector<int16_t> pcm; int sample_rate; };
                    constexpr int PCM_QUEUE_LEN = 8;
                    QueueHandle_t pcm_queue = xQueueCreate(PCM_QUEUE_LEN, sizeof(PcmPacket*));

                    // Heap-allocate shared state so both tasks outlive the decode task's stack
                    auto* decode_done = new std::atomic<bool>{false};
                    auto* abort_flag  = new std::atomic<bool>{false};

                    // Output task: dequeues PCM packets and sends to codec
                    struct OutputParams {
                        QueueHandle_t q;
                        std::atomic<bool>* decode_done;
                        std::atomic<bool>* abort_flag;
                    };
                    auto* op = new OutputParams{pcm_queue, decode_done, abort_flag};
                    xTaskCreate([](void* arg) {
                        auto* op = static_cast<OutputParams*>(arg);
                        auto& app = Application::GetInstance();

                        while (true) {
                            PcmPacket* pkt = nullptr;
                            if (xQueueReceive(op->q, &pkt, pdMS_TO_TICKS(200)) != pdTRUE) {
                                if (op->decode_done->load()) break;
                                continue;
                            }

                            if (op->abort_flag->load()) {
                                delete pkt;
                                continue;
                            }

                            // Wait until Idle; abort only if user actively interrupts (Listening)
                            while (!op->abort_flag->load()) {
                                auto state = app.GetDeviceState();
                                if (state == kDeviceStateIdle) break;
                                if (state == kDeviceStateListening) {
                                    op->abort_flag->store(true);
                                    break;
                                }
                                // Speaking (TTS reply) — wait it out
                                vTaskDelay(pdMS_TO_TICKS(50));
                            }

                            if (op->abort_flag->load()) {
                                delete pkt;
                                continue;
                            }

                            AudioStreamPacket ap;
                            ap.sample_rate = pkt->sample_rate;
                            ap.frame_duration = 60;
                            ap.payload.resize(pkt->pcm.size() * sizeof(int16_t));
                            memcpy(ap.payload.data(), pkt->pcm.data(), ap.payload.size());
                            app.AddAudioData(std::move(ap));
                            delete pkt;
                        }

                        // Drain remaining packets
                        PcmPacket* pkt = nullptr;
                        while (xQueueReceive(op->q, &pkt, 0) == pdTRUE) delete pkt;
                        vQueueDelete(op->q);
                        delete op->decode_done;
                        delete op->abort_flag;
                        delete op;
                        vTaskDelete(nullptr);
                    }, "music_out", 4096, op, 5, nullptr);

                    // Decode task (current task): HTTP read → OggDemuxer → Opus → PCM queue
                    void* music_decoder = nullptr;
                    esp_opus_dec_cfg_t dec_cfg = {
                        .sample_rate    = 48000,
                        .channel        = ESP_AUDIO_MONO,
                        .frame_duration = (esp_opus_dec_frame_duration_t)ESP_OPUS_ENC_FRAME_DURATION_60_MS,
                        .self_delimited = false,
                    };
                    esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &music_decoder);
                    if (!music_decoder) {
                        ESP_LOGE(TAG, "music.play: failed to create opus decoder");
                        http->Close();
                        abort_flag->store(true);
                        decode_done->store(true);
                        vTaskDelete(nullptr);
                        return;
                    }

                    int current_sample_rate = 48000;

                    auto demuxer = std::make_unique<OggDemuxer>();
                    demuxer->OnDemuxerFinished([&, abort_flag, decode_done, pcm_queue](const uint8_t* data, int sample_rate, size_t len) {
                        if (abort_flag->load()) return;

                        // Reconfigure decoder if sample rate changed
                        if (sample_rate != current_sample_rate) {
                            current_sample_rate = sample_rate;
                            esp_opus_dec_close(music_decoder);
                            music_decoder = nullptr;
                            esp_opus_dec_cfg_t new_cfg = {
                                .sample_rate    = (uint32_t)sample_rate,
                                .channel        = ESP_AUDIO_MONO,
                                .frame_duration = (esp_opus_dec_frame_duration_t)ESP_OPUS_ENC_FRAME_DURATION_60_MS,
                                .self_delimited = false,
                            };
                            esp_opus_dec_open(&new_cfg, sizeof(new_cfg), &music_decoder);
                            if (!music_decoder) { abort_flag->store(true); return; }
                        }

                        // Decode Opus → PCM
                        constexpr size_t MAX_PCM_SAMPLES = 48000 / 1000 * 60;
                        auto* pkt = new PcmPacket();
                        pkt->pcm.resize(MAX_PCM_SAMPLES);
                        pkt->sample_rate = current_sample_rate;

                        esp_audio_dec_in_raw_t raw = {
                            .buffer        = const_cast<uint8_t*>(data),
                            .len           = (uint32_t)len,
                            .consumed      = 0,
                            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                        };
                        esp_audio_dec_out_frame_t out_frame = {
                            .buffer       = (uint8_t*)pkt->pcm.data(),
                            .len          = (uint32_t)(pkt->pcm.size() * sizeof(int16_t)),
                            .decoded_size = 0,
                        };
                        esp_audio_dec_info_t dec_info = {};
                        auto ret = esp_opus_dec_decode(music_decoder, &raw, &out_frame, &dec_info);
                        if (ret != ESP_AUDIO_ERR_OK) {
                            ESP_LOGW(TAG, "music.play: opus decode err %d", ret);
                            delete pkt;
                            return;
                        }
                        pkt->pcm.resize(out_frame.decoded_size / sizeof(int16_t));

                        // Block until queue has space or abort; drop frames during Speaking
                        while (!abort_flag->load()) {
                            auto state = Application::GetInstance().GetDeviceState();
                            if (state == kDeviceStateListening) {
                                abort_flag->store(true);
                                break;
                            }
                            if (state == kDeviceStateSpeaking) {
                                // TTS playing — drop this frame to keep HTTP flowing
                                delete pkt;
                                pkt = nullptr;
                                break;
                            }
                            if (xQueueSend(pcm_queue, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
                                pkt = nullptr;
                                break;
                            }
                        }
                        if (pkt) delete pkt;
                    });
                    demuxer->Reset();

                    constexpr size_t BUF_SIZE = 4096;
                    std::vector<uint8_t> buf(BUF_SIZE);
                    while (!abort_flag->load()) {
                        int n = http->Read(reinterpret_cast<char*>(buf.data()), BUF_SIZE);
                        if (n <= 0) break;
                        demuxer->Process(buf.data(), n);
                    }
                    http->Close();
                    if (music_decoder) esp_opus_dec_close(music_decoder);
                    decode_done->store(true);
                    ESP_LOGI(TAG, "music.play: decode %s '%s'", abort_flag->load() ? "aborted" : "done", title.c_str());
                    vTaskDelete(nullptr);
                }, "music_dec", 16384, p, 5, nullptr);

                return std::string("正在播放: ") + title;
            });
    }

    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,  // 不初始化新的 SCCB，使用现有的 I2C 总线
            .i2c_handle = i2c_bus_,  // 使用现有的 I2C 总线句柄
            .freq = 100000,  // 100kHz
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = 12000000,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);

    }
public:
    CustomBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeTca9555();
        InitializeSpi();
        InitializeButtons();
        #ifdef LCD_TYPE_JD9853_SERIAL
        InitializeJd9853Display(); 
        #else
        InitializeSt7789Display(); 
        #endif
        InitializeCamera();
        GetBacklight()->RestoreBrightness();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 6);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
            return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, BACKLIGHT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(CustomBoard);
