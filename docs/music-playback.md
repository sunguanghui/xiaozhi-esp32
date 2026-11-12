# Music Playback (Fork Feature)

This fork adds server-driven music playback on top of the standard voice
assistant flow. The server (through the `music` message type or the built-in
MCP tools) can start, stop, and report on a Bilibili audio stream while the
device is idle.

## Data Flow

```
server "music" message / MCP tool
        |
        v
Application::HandleMusicMessage()          (network/MCP thread)
        |
        |  Schedule()  -- state is only mutated on the main task
        v
Application::Run() main event loop
        |
        v
AudioService::PlayMusicFromUrl()
        |
        v
MusicTask (own FreeRTOS task)
   HTTP -> OggDemuxer -> Opus decoder -> optional resampler
        |
        v
audio_playback_queue_ -> AudioOutputTask -> codec
```

`Application::HandleMusicMessage()` may run on the MQTT/WebSocket or MCP
thread. It never touches `AudioService` directly; every playback mutation is
posted with `Application::Schedule()`.

## Playback Lifecycle

- Each `PlayMusicFromUrl()` increments `music_generation_` and starts a new
  `MusicTask(generation)`. A task is stale as soon as the generation changes,
  so a stopping task can never clear the new stream's state or close its
  decoder.
- `MusicTask` instances are serialized by `music_control_mutex_`, so the new
  stream does not open a second HTTP connection or touch the codec while the
  previous stream is still unwinding.
- The Opus decoder and resampler are local to each `MusicTask`, not shared
  members.
- Music frames carry `AudioTask::is_music`; `ResetDecoder()` keeps those frames
  when TTS decoding is reset, and `StopMusic()` drops them so playback stops
  immediately.
- PCM buffers for music frames are returned to a small recycle pool by
  `AudioOutputTask`, avoiding a heap allocation per Opus packet.

## Pause / Resume

1. A wake word pauses music (`AudioService::PauseMusic()`): the position is
   saved and the stream is stopped. If a new song is started instead, any saved
   position is discarded.
2. When the assistant finishes speaking (`Speaking -> Listening`) the
   application starts a one-shot 3-second timer. Any state change cancels it.
3. If the timer fires while the device is still listening, no speech has been
   detected, and paused music exists, the client closes the audio channel and
   enters idle. Idle then resumes the track with `start_time=<seconds>`.
4. A server-initiated `goodbye` never resumes music. The pause state is
   discarded shortly after the device settles in idle, so a later shutdown
   cannot restart the previous track.

Resume position is measured from PCM actually written to the codec
(`music_played_ms_`, using the frame size and output sample rate), plus the
`start_time` offset the stream was started from. Queued frames that never
reached the speaker are not counted.

## Configuration

The music proxy base URL is a Kconfig option:

```
CONFIG_MUSIC_API_BASE_URL="http://8.153.171.156:18000"
```

`main/application.cc` falls back to that value if the symbol is not generated.
Prefer an HTTPS endpoint for anything beyond local testing.

## Logging

Signed stream URLs are time-limited credentials. Only the scheme, host, and
path are logged; the query string is replaced with `?<redacted>`.

## Related MCP Tools

- `self.music.search_and_play` and `self.music.play_url` return `true` when the
  asynchronous playback request was accepted, not when audio is already
  playing.
- `self.music.stop` stops playback and clears the saved position.
- `self.music.get_status` reports `{playing, paused}` thread-safely.

## Manual Validation

A successful build is not validation of this feature. On hardware, verify:

- Play a track, say the wake word, finish a conversation: music resumes about
  three seconds after the assistant stops speaking, at roughly the same
  position (within one audio frame).
- While music is paused, ask for a different song: only the new song plays.
- Let the server end the session (shutdown/timeout) while music is paused:
  music does not resume afterwards.
- Play music, then ask the server to stream a TTS reply: the audio channel is
  closed and TTS does not talk over the music.
- Run a long track and watch SRAM/`heap_caps` over several minutes: memory
  should stay flat (pooled buffers, one task per stream).
- Exercise both MQTT/UDP and WebSocket transports if both are built.
