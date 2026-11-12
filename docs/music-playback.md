# Music, Audiobook, and Radio Playback (Fork Feature)

This fork adds server-driven music, audiobook, and live radio playback on top
of the standard voice assistant flow. The server (through the `music` /
`audiobook` / `radio` message types or the built-in MCP tools) can start, stop,
and report on a stream while the device is idle.

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

The music and radio proxy base URLs are Kconfig options:

```
CONFIG_MUSIC_API_BASE_URL="http://8.153.171.156:18000"
CONFIG_RADIO_API_BASE_URL="http://8.153.171.156:12300"
```

`main/application.cc` falls back to those values if the symbols are not
generated. Prefer HTTPS endpoints for anything beyond local testing.

## Logging

Signed stream URLs are time-limited credentials. Only the scheme, host, and
path are logged; the query string is replaced with `?<redacted>`.

## Related MCP Tools

- `self.music.search_and_play` and `self.music.play_url` return `true` when the
  asynchronous playback request was accepted, not when audio is already
  playing.
- `self.music.stop` stops playback and clears the saved position.
- `self.music.get_status` reports `{playing, paused}` thread-safely.

## Audiobooks

An audiobook is a multi-episode Bilibili album. It uses exactly the same
streaming, decoding, pause/resume, and session-close path as music; the only
extra state is which album and episode the user is in.

Upstream endpoints (additive; `/audio/search` is unchanged):

- `GET /audio/search?keyword=<name>&episode=<n>` — search and resolve an
  episode in one step.
- `GET /audio/resolve?bvid=<bv>&episode=<n>` — resolve an episode of a known
  album without searching again. Same response shape as `/audio/search`.
- `GET /audio/info?bvid=<bv>` — album title and the episode list
  (`{episode, title, duration}`), without resolving an audio stream.

The device converts `play_info.play_url` from `/music/proxy` to
`/music/opus_stream`, then calls `AudioService::PlayMusicFromUrl()` on the
main task. Album state (`bvid`, title, current/total episodes) is guarded by
`audiobook_mutex_` and cleared when a plain track starts or when the
`self.audiobook.stop` tool is used.

`audiobook_generation_` invalidates a result whose HTTP request was superseded
by another album, a plain track, or a stop while it was in flight.

### Device MCP tools

- `self.audiobook.search_and_play(keyword, episode=1)` — search and play.
- `self.audiobook.next` — play the next episode; returns `false` at the last
  episode or when no album is loaded.
- `self.audiobook.play_episode(episode)` — jump to an episode of the loaded
  album.
- `self.audiobook.get_status` — `{album_title, bvid, current_episode,
  total_episodes, playing, paused}`.
- `self.audiobook.stop` — stop playback and forget the album. Plain
  `self.music.stop` stops playback but keeps the album, so `next` can continue.

### Server message type

The same operations are available through the `audiobook` JSON message type:

```json
{"type": "audiobook", "action": "search",  "keyword": "明朝那些事儿 有声书", "episode": 1}
{"type": "audiobook", "action": "resolve", "bvid": "BV11C736WEJv", "episode": 2}
{"type": "audiobook", "action": "next"}
{"type": "audiobook", "action": "stop"}
```

## Live Radio

Live radio is a continuous live stream, not a track with a position. It reuses
the music streaming path end to end; the only extra state is the station list
from the last search.

The radio service (`CONFIG_RADIO_API_BASE_URL`) can only serve MP3/AAC/HLS
station URLs, which the device cannot decode. Two additive upstream endpoints
solve that (the existing `/api/radio/search` is unchanged):

- `GET /api/radio/play?q=<keyword>&limit=<n>` — search and return stations
  with `play_url` replaced by a signed `/api/radio/opus_stream` URL. Response
  shape is `{status, total, query, data: [{name, play_url, ...}]}`.
- `GET /api/radio/opus_stream?target=<url>&sig=<hmac>` — transcode the station
  with ffmpeg to Ogg/Opus (mono, 48 kHz) and stream it. Requests without a
  valid `sig` are rejected; the ffmpeg child is killed when the client
  disconnects.

The radio service image must include `ffmpeg` (see its `Dockerfile`). Set the
service's `PROXY_SECRET` environment variable to a fixed value; otherwise each
process start generates a random secret and play URLs issued before a restart
are rejected. The signed play URL is built from the request's `base_url`, so the
device must reach the service at the same host/port the service sees.

On the device, `Application::RadioRequestTask()` fetches
`/api/radio/play` in a background task, keeps up to 10 stations, and schedules
the first one through `AudioService::PlayMusicFromUrl()`. `radio_generation_`
drops a result superseded by another search, a stop, or music/audiobook
playback while the request was in flight.

Because the stream is live, the resume position saved by the wake-word pause is
meaningless to the server: resuming restarts the current station from live
audio. The station list survives a pause, so `next` still works afterwards.

Starting music, an audiobook, or a radio search clears the other two contexts,
so `self.audiobook.next` returns `false` after a song starts and
`self.radio.next` returns `false` after an audiobook starts.

### Device MCP tools

- `self.radio.search_and_play(keyword)` — search and play the first station.
- `self.radio.next` — switch to the next station from the last search results
  (wraps around); returns `false` when no list is loaded.
- `self.radio.get_status` — `{active, query, station_name, station_index,
  total_stations, playing, paused}` (`station_index` is 1-based).
- `self.radio.stop` — stop playback and forget the station list.

### Server message type

```json
{"type": "radio", "action": "play", "q": "中国之声"}
{"type": "radio", "action": "next"}
{"type": "radio", "action": "stop"}
```

`action` accepts `play` (alias `search`); the query may be in `q`, `query`, or
`keyword`.

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
- Play an audiobook and use `self.audiobook.next` several times: episodes
  advance without restarting the album from episode 1, and `current_episode`
  in `get_status` matches what is heard.
- Pause an audiobook with the wake word, finish the conversation: playback
  resumes at roughly the same position, with the album state intact.
- Ask for a normal song after listening to an audiobook, then use
  `self.audiobook.next`: it must return `false` (the album was replaced).
- Play a radio station, wait several minutes: audio stays continuous (no
  decoder/HTTP errors), and heap stays flat.
- Use `self.radio.next` several times: stations advance and wrap around, and
  `get_status` reports the station actually heard.
- Pause radio with the wake word, finish the conversation: the app resumes the
  station from live audio (not the pre-pause position), and `next` still works.
- Stop radio, then ask for a song or audiobook, then call `self.radio.next`:
  it must return `false` (the station list was replaced).
