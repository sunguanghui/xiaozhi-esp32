# 音乐播放工具已添加

## 问题描述

之前当用户说"播放周杰伦稻香"时，AI 会：
1. 调用 `search_chat()` 搜索版权信息
2. 回复"没办法直接播放...版权的问题"
3. **从来没有尝试调用播放音乐的功能**

## 根本原因

ESP32 设备**有**播放音乐的能力（通过 `HandleMusicMessage` 和 B站音乐 API），但是：
- ❌ 没有在 MCP Server 中注册音乐播放工具
- ❌ AI 不知道设备可以播放音乐
- ❌ AI 只能用 Dify 自己的 `search_chat` 工具

## 解决方案

在 `main/mcp_server.cc` 的 `AddCommonTools()` 函数中添加了 4 个新的 MCP 工具：

### 1. `self.music.search_and_play`
搜索并播放 B站音乐（第一个搜索结果）

**参数：**
- `keyword` (必需): 搜索关键词（歌名、歌手名或两者）
- `time_range` (可选): 时间范围过滤，可选值：'day', 'week', 'month', 'all'，默认 'week'

**示例：**
```json
{
  "keyword": "稻香 周杰伦",
  "time_range": "week"
}
```

### 2. `self.music.play_url`
直接播放指定 URL 的音乐

**参数：**
- `url` (必需): 音频文件或流的 URL（支持 mp3, ogg, opus 等）

**示例：**
```json
{
  "url": "https://example.com/music.mp3"
}
```

### 3. `self.music.stop`
停止当前播放的音乐

**参数：** 无

### 4. `self.music.get_status`
获取音乐播放状态

**参数：** 无

**返回：**
```json
{
  "playing": true
}
```

## 技术实现

这些工具通过调用现有的 `Application::HandleMusicMessage()` 方法来实现，该方法会：
1. 对于 `search_and_play`：调用 B站音乐 API 搜索并播放
2. 对于 `play_url`：直接播放指定 URL
3. 对于 `stop`：调用 `AudioService::StopMusic()`
4. 对于 `get_status`：调用 `AudioService::IsMusicPlaying()`

## 编译和刷写

### 方法 1：使用 idf.py（推荐）
```bash
# 配置开发板（替换为你的开发板名称）
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.board.你的开发板名" build

# 刷写到设备
idf.py flash
```

### 方法 2：使用已有的构建脚本
如果项目有 Makefile 或其他构建脚本，按照之前的方式构建即可。

## 使用示例

升级固件后，用户可以直接说：
- "播放周杰伦的稻香"
- "播放告白气球"
- "停止播放"
- "现在在播放音乐吗？"

AI 会自动调用相应的 `self.music.*` 工具来执行操作。

## 修改的文件

- `main/mcp_server.cc` - 添加了 4 个音乐相关的 MCP 工具

## 注意事项

1. **需要重新编译和刷写固件**才能生效
2. 音乐播放依赖 B站音乐 API（`kBilibiliMusicApiBase`），确保设备有网络连接
3. 播放会在设备进入对话状态时自动停止（对话优先级更高）
4. 支持的音频格式取决于设备的音频解码器配置

## 测试建议

刷写新固件后，测试以下场景：
1. ✅ "播放周杰伦稻香" - 应该调用 `self.music.search_and_play`
2. ✅ "停止播放" - 应该调用 `self.music.stop`
3. ✅ "正在播放音乐吗" - 应该调用 `self.music.get_status`
4. ✅ 对话时音乐自动暂停（已有功能）
