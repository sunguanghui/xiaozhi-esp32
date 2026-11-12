#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <atomic>
#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)
#define MAIN_EVENT_MUSIC_RESUME_CHECK   (1 << 14)
#define MAIN_EVENT_SERVER_GOODBYE_CHECK (1 << 15)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

// Album/episode information for the audiobook currently loaded by the user.
// Audiobooks are streamed through the same AudioService path as music.
struct AudiobookStatus {
    std::string album_title;
    std::string bvid;
    int current_episode = 0;  // 1-based, 0 when no album is loaded
    int total_episodes = 0;
};

// A live radio station returned by the radio service. play_url is a signed
// Ogg/Opus stream, so it can be handed to AudioService directly.
struct RadioStation {
    std::string name;
    std::string play_url;
};

struct RadioStatus {
    std::string query;
    std::string station_name;
    int station_index = 0;  // 1-based current station, 0 when none
    int total_stations = 0;
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

    /**
     * Handle music playback message
     * Can be called from MCP tools to play music
     */
    void HandleMusicMessage(const cJSON* root);

    /**
     * Handle an audiobook message (server "audiobook" type or MCP tools).
     * Actions: search, resolve, next, stop.
     * Safe to call from any task; HTTP work runs in a background task.
     */
    void HandleAudiobookMessage(const cJSON* root);

    /**
     * Search Bilibili for an audiobook and play the given episode (1-based).
     * Safe to call from any task.
     */
    void SearchAndPlayAudiobook(const std::string& keyword, int episode = 1);

    /**
     * Play an episode of the album identified by bvid (1-based).
     * Safe to call from any task.
     */
    void PlayAudiobookEpisode(const std::string& bvid, int episode = 1);

    /**
     * Play the next episode of the currently loaded audiobook album.
     * Returns false when no album is loaded or the last episode is current.
     */
    bool PlayNextAudiobookEpisode();

    /**
     * Copy the currently loaded album information. Returns false when no
     * album has been loaded (or after the audiobook was stopped).
     */
    bool GetAudiobookStatus(AudiobookStatus* status) const;

    /**
     * Forget the loaded album and discard any in-flight audiobook request.
     * Safe to call from any task.
     */
    void ClearAudiobookStatus();

    /**
     * Handle a radio message (server "radio" type or MCP tools).
     * Actions: play, next, stop.
     * Safe to call from any task; HTTP work runs in a background task.
     */
    void HandleRadioMessage(const cJSON* root);

    /**
     * Search the radio service and play the first live station.
     * Safe to call from any task.
     */
    void SearchAndPlayRadio(const std::string& query);

    /**
     * Switch to the next station from the last search results (wraps around).
     * Returns false when no station list is loaded.
     */
    bool PlayNextRadioStation();

    /**
     * Copy the current radio status. Returns false when no station is loaded.
     */
    bool GetRadioStatus(RadioStatus* status) const;

    /**
     * Forget the current station list and discard any in-flight radio request.
     * Safe to call from any task.
     */
    void ClearRadioStatus();

    /**
     * Permanently stop music and cancel any pending wake-word resume.
     * Safe to call from any task.
     */
    void StopMusicPermanently();

    /**
     * Notify the application that the server ended the session (goodbye).
     * A server-initiated session end must not auto-resume paused music.
     * Safe to call from any task.
     */
    void NotifyServerGoodbye();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    // Music resume is only allowed for a session the client closed on purpose
    // (the 3-second silence path). Server-initiated ends do not resume music.
    bool resume_music_after_session_ = false;
    esp_timer_handle_t music_resume_timer_ = nullptr;
    esp_timer_handle_t server_goodbye_timer_ = nullptr;
    std::atomic<bool> server_goodbye_pending_{false};
    // Audiobook album state, used by the "next episode" tool. Guarded by
    // audiobook_mutex_ because MCP/network threads read it while background
    // requests update it. audiobook_generation_ invalidates in-flight results
    // when the user starts another album/track or stops playback.
    mutable std::mutex audiobook_mutex_;
    uint32_t audiobook_generation_ = 0;
    std::string audiobook_keyword_;
    std::string audiobook_bvid_;
    std::string audiobook_album_title_;
    int audiobook_current_episode_ = 0;
    int audiobook_total_episodes_ = 0;
    // Live radio state. The list holds the stations (with their signed Opus
    // play URLs) from the last search so "next station" does not search again.
    mutable std::mutex radio_mutex_;
    uint32_t radio_generation_ = 0;
    std::string radio_query_;
    std::vector<RadioStation> radio_stations_;
    int radio_station_index_ = 0;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    DeviceState previous_state_{kDeviceStateUnknown};  // For detecting Speaking->Listening transition


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void HandleMusicResumeCheckEvent();
    void HandleServerGoodbyeCheckEvent();
    void CancelPendingMusicResume();
    void StartAudiobookRequest(bool search_by_keyword, const std::string& query, int episode);
    void AudiobookRequestTask(bool search_by_keyword, std::string query, int episode,
                              uint32_t generation);
    void StartRadioRequest(const std::string& query);
    void RadioRequestTask(std::string query, uint32_t generation);
    void CloseMusicSessionSoon();
    void CloseMusicSession();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;

    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
