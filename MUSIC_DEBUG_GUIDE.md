# 音乐播放问题诊断指南

## 🔍 问题现状

**症状：** 音乐播放 3-4 秒后停止，没有声音
**已尝试修复：**
- ✅ 添加了 MCP 音乐工具
- ✅ 修改了状态转换逻辑
- ✅ 修改了 ResetDecoder 保留音乐任务
- ❌ **仍然只播放 3-4 秒**

## 📊 需要收集的日志

### 最新固件（commit `8c9b663`）包含详细日志

刷写最新固件后，测试"播放周杰伦稻香"，然后收集以下日志：

### 1. 音乐任务日志

查找以下关键日志：

```
I (xxxxx) AudioService: MusicTask: start streaming 'http://...'
D (xxxxx) AudioService: MusicTask: decoded XXXX PCM samples, pushing to queue
D (xxxxx) AudioService: MusicTask: pushed to queue, queue_size=X
D (xxxxx) AudioService: MusicTask: downloaded XXX KB
I (xxxxx) AudioService: MusicTask: streaming finished, total=XXX KB, aborted=X
```

**关键问题：**
- `MusicTask` 是否持续下载数据？
- `aborted` 是 0 还是 1？
- 总下载量是多少？（应该是几百 KB 到几 MB）

### 2. ResetDecoder 日志

查找：

```
I (xxxxx) AudioService: ResetDecoder: music_playing=true, queue before=X, after=Y, removed=Z TTS tasks
```

或

```
I (xxxxx) AudioService: ResetDecoder: music_playing=false, cleared all X tasks
```

**关键问题：**
- `music_playing` 是 true 还是 false？
- 队列清空了多少任务？
- 是否有音乐任务被错误清空？

### 3. StopMusic 日志

查找：

```
I (xxxxx) AudioService: StopMusic called
```

**关键问题：**
- `StopMusic` 是否被调用？
- 什么时候被调用的？（播放几秒后？）

### 4. 完整日志示例

**正常情况应该是：**
```
I AudioService: MusicTask: start streaming 'http://...'
I AudioService: PlayMusicFromUrl: music_playing set to true
D AudioService: MusicTask: decoded 2304 PCM samples, pushing to queue
D AudioService: MusicTask: pushed to queue, queue_size=1
D AudioService: MusicTask: decoded 2304 PCM samples, pushing to queue
D AudioService: MusicTask: pushed to queue, queue_size=2
... (持续推送)
D AudioService: MusicTask: downloaded 100 KB
D AudioService: MusicTask: downloaded 200 KB
... (持续下载到歌曲结束)
I AudioService: MusicTask: streaming finished, total=3500 KB, aborted=0
```

**异常情况可能是：**
```
I AudioService: MusicTask: start streaming 'http://...'
D AudioService: MusicTask: decoded 2304 PCM samples, pushing to queue
D AudioService: MusicTask: pushed to queue, queue_size=1
I AudioService: ResetDecoder: music_playing=false, cleared all 5 tasks  ← 问题！
I AudioService: StopMusic called  ← 问题！
W AudioService: MusicTask: task dropped (aborted)  ← 问题！
I AudioService: MusicTask: streaming finished, total=15 KB, aborted=1  ← 问题！
```

## 🔧 如何收集日志

### 方法 1：通过串口监视器

```bash
idf.py monitor
```

或使用任何串口工具连接到设备，波特率 115200。

### 方法 2：通过 ESP-IDF 日志

如果使用 VS Code + ESP-IDF 扩展：
1. 点击 "ESP-IDF: Monitor Device"
2. 保存日志到文件

### 方法 3：使用 esptool

```bash
python -m serial.tools.miniterm COM3 115200 > music_debug.log
```

## 📝 完整测试步骤

1. **刷写最新固件**（commit `8c9b663`）
2. **连接串口监视器**
3. **开始记录日志**
4. **重启设备**
5. **说："小智，播放周杰伦稻香"**
6. **等待 10 秒**（观察音乐是否停止）
7. **停止记录，保存日志**
8. **发送日志给我分析**

## 🎯 重点关注

### 关键指标

| 指标 | 正常值 | 异常值 |
|------|--------|--------|
| `music_playing` | true | false |
| `total_bytes` | > 1000 KB | < 100 KB |
| `aborted` | 0 | 1 |
| `queue_size` | 持续有数据 | 很快变为 0 |
| `StopMusic called` | 不出现 | 出现 |

### 可能的问题

#### 场景 A：music_playing 是 false
```
ResetDecoder: music_playing=false, cleared all X tasks
```
**原因：** `music_playing_` 状态管理有问题  
**解决：** 检查 `PlayMusicFromUrl` 和 `StopMusic` 的调用时机

#### 场景 B：MusicTask 被 abort
```
MusicTask: streaming finished, total=15 KB, aborted=1
```
**原因：** `music_abort_` 被设置为 true  
**解决：** 检查谁调用了 `StopMusic` 或设置了 abort

#### 场景 C：队列被清空
```
ResetDecoder: music_playing=true, queue before=10, after=0
```
**原因：** 音乐任务的 `timestamp` 不是 0  
**解决：** 检查 MusicTask 是否正确设置 `timestamp = 0`

#### 场景 D：下载停止
```
MusicTask: downloaded 15 KB
(没有更多下载日志)
```
**原因：** HTTP 连接断开或 abort  
**解决：** 检查网络连接或 `music_abort_` 状态

## 💡 临时测试方案

如果无法获取完整日志，可以尝试：

### 测试 1：直接播放 URL
```cpp
self.music.play_url({"url": "http://example.com/test.ogg"})
```
使用一个小的测试音频文件（< 1MB），看是否能完整播放。

### 测试 2：禁用 ResetDecoder
临时注释掉 `application.cc:971` 的 `ResetDecoder()` 调用：
```cpp
case kDeviceStateSpeaking:
    ...
    // audio_service_.ResetDecoder();  // 临时禁用
```
看音乐是否能继续播放。

### 测试 3：检查 music_playing_ 状态
在播放音乐后，调用 `self.music.get_status()`，确认返回 `{"playing": true}`。

## 📤 发送日志

请将完整的串口日志发送给我，特别是包含：
- `MusicTask: start streaming`
- `ResetDecoder`
- `StopMusic`
- `MusicTask: streaming finished`

这些信息能帮助我准确定位问题。

---
创建时间: 2026-07-30  
固件版本: commit 8c9b663  
目的: 诊断音乐播放 3-4 秒后停止的问题
