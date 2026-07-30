# 音乐播放问题 - 当前状态报告

## 📊 项目状态

**最新 Commit:** `ab3c8c7`  
**状态:** 🔍 等待日志诊断  
**问题:** 音乐仍然只播放 3-4 秒后停止

---

## ✅ 已完成的工作

### 1. 添加 MCP 音乐工具 (commit `73b729b`)

添加了 4 个 MCP 工具到 `main/mcp_server.cc`：
- ✅ `self.music.search_and_play` - 搜索并播放 B站音乐
- ✅ `self.music.play_url` - 直接播放 URL
- ✅ `self.music.stop` - 停止播放
- ✅ `self.music.get_status` - 获取播放状态

**结果:** AI 现在可以调用播放工具，不再说"没办法播放"

### 2. 修复状态转换逻辑 (commit `ca4c088`)

修改 `main/application.cc:913-918`：
```cpp
// 旧版：任何非 Idle 状态都停止音乐
if (new_state != kDeviceStateIdle && new_state != kDeviceStateUnknown) {
    audio_service_.StopMusic();
}

// 新版：只在用户说话时停止音乐
if (new_state == kDeviceStateListening) {
    audio_service_.StopMusic();
}
```

**结果:** AI 回复时不会立即停止音乐

### 3. 修复 ResetDecoder 清空队列 (commit `c198cb5`)

修改 `main/audio/audio_service.cc:756-769`：
```cpp
// 根据 music_playing_ 决定清空策略
if (music_playing_.load()) {
    // 只移除 TTS 任务（timestamp != 0），保留音乐任务
    audio_playback_queue_.erase(
        std::remove_if(..., [](const auto& task) {
            return task->timestamp != 0;
        }), ...);
} else {
    audio_playback_queue_.clear();
}
```

**结果:** ResetDecoder 应该保留音乐任务

### 4. 添加详细调试日志 (commit `8c9b663`)

在 `main/audio/audio_service.cc` 添加了：
- ✅ ResetDecoder 队列状态日志
- ✅ MusicTask PCM 推送日志
- ✅ HTTP 下载进度日志
- ✅ 任务 abort 状态日志

**结果:** 可以追踪完整的音乐播放流程

---

## ❌ 未解决的问题

### 问题描述

**症状:** 音乐播放约 3-4 秒后完全停止，没有声音

**用户反馈:**
> "为什么重新刷入固件后，情况还是一样啊，一直只播放歌曲前面的3秒，然后就彻底没有声音了。"

### 可能的原因

基于代码分析，可能的问题：

#### 假设 A: `music_playing_` 状态未正确设置
- `PlayMusicFromUrl` 设置 `music_playing_ = true`
- 但可能在 AI 回复前被其他地方重置为 false
- 导致 ResetDecoder 清空所有任务

#### 假设 B: `music_abort_` 被意外触发
- 某个地方调用了 `StopMusic()`
- 或者直接设置了 `music_abort_ = true`
- 导致 MusicTask 停止下载

#### 假设 C: 音乐任务的 `timestamp` 不是 0
- 虽然代码设置了 `task->timestamp = 0`
- 但可能在某个地方被修改
- 导致被 ResetDecoder 错误清除

#### 假设 D: HTTP 连接问题
- 网络不稳定导致连接断开
- 但 API 日志显示正常请求流
- 可能性较低

#### 假设 E: 队列被其他地方清空
- `Stop()` 方法在 line 180 也清空队列
- 可能有其他未发现的清空点

---

## 🔍 下一步诊断

### 需要的信息

**关键日志：** 刷写最新固件（commit `ab3c8c7`），测试播放音乐，收集完整串口日志

**重点查找：**
1. `MusicTask: start streaming` - 确认任务启动
2. `ResetDecoder: music_playing=X` - 确认状态
3. `MusicTask: pushed to queue` - 确认入队
4. `MusicTask: streaming finished, aborted=X` - 确认是否被中止
5. `StopMusic called` - 确认是否被调用

### 诊断文档

已创建 `MUSIC_DEBUG_GUIDE.md`，包含：
- 📋 完整的日志收集步骤
- 🔍 关键日志模式
- 📊 正常 vs 异常行为对比
- 🎯 可能的问题场景分析

---

## 📂 相关文件

### 修改的源代码
- `main/mcp_server.cc` - MCP 音乐工具
- `main/application.h` - HandleMusicMessage 公有化
- `main/application.cc` - 状态转换逻辑
- `main/audio/audio_service.cc` - ResetDecoder 修复 + 调试日志

### 文档
- `MUSIC_TOOLS_ADDED.md` - 工具添加说明
- `MUSIC_PLAYBACK_FIX.md` - 第一次修复报告
- `MUSIC_PLAYBACK_ROOT_CAUSE_FIX.md` - 根本原因分析
- `MUSIC_DEBUG_GUIDE.md` - 诊断指南 ⭐

---

## 🎯 Git 历史

```
ab3c8c7 docs: add comprehensive music playback debugging guide
8c9b663 debug: add detailed logging for music playback troubleshooting
1ee3fb9 docs: add root cause analysis for music playback issue
c198cb5 fix: preserve music playback when ResetDecoder is called during TTS
ca4c088 fix: only stop music when user starts listening, not during AI speech
73b729b feat: add music playback MCP tools
```

---

## 💡 临时测试建议

### 快速验证 1：禁用 ResetDecoder

编辑 `main/application.cc:971`：
```cpp
case kDeviceStateSpeaking:
    display->SetStatus(Lang::Strings::SPEAKING);
    // ... 其他代码 ...
    // audio_service_.ResetDecoder();  // ← 临时注释掉
    break;
```

重新编译刷写，测试音乐是否能完整播放。

### 快速验证 2：检查 music_playing 状态

在播放音乐后立即调用：
```
self.music.get_status()
```

应该返回 `{"playing": true}`，如果返回 false，说明状态管理有问题。

---

## 📈 预期 vs 实际

### 预期行为 ✅
```
用户: "播放周杰伦稻香"
AI: 调用 self.music.search_and_play
设备: 下载并播放完整歌曲（3-4 分钟）
日志: 持续的 "MusicTask: pushed to queue"
结果: 音乐正常播放
```

### 实际行为 ❌
```
用户: "播放周杰伦稻香"
AI: 调用 self.music.search_and_play ✅
设备: 播放 3-4 秒后停止 ❌
日志: ??? (需要收集)
结果: 音乐中断
```

---

## 🚀 行动项

### 立即执行
1. ✅ 代码已推送到 GitHub
2. ⏳ 等待 GitHub Actions 构建固件
3. 📥 下载最新固件（commit `ab3c8c7`）
4. 📱 刷写到设备
5. 🔍 收集完整串口日志
6. 📤 分析日志找出问题

### 等待信息
- [ ] 串口日志（完整）
- [ ] `music_playing` 状态
- [ ] `music_abort` 是否被触发
- [ ] ResetDecoder 的行为
- [ ] 队列大小变化

---

## 📝 总结

我们已经完成了：
- ✅ 理论上的所有修复
- ✅ 详细的调试日志
- ✅ 完整的诊断文档

但问题依然存在，说明：
- ⚠️ 可能有未发现的清空点
- ⚠️ 或者状态管理逻辑有问题
- ⚠️ 需要实际日志来定位

**下一步：收集日志，精确定位问题所在。**

---
状态更新时间: 2026-07-30  
最新固件: commit ab3c8c7  
等待: 用户提供串口日志
