# Audio (audout:u)

`src/core/hle/services/audio.*`. This is the simple output that libnx's `audoutInitialize` uses, and SDL2 for Switch uses it too.
`audren:u` (the audio renderer) is not implemented yet: it still answers "not available".

## Commands

| Object | Commands |
|---|---|
| `audout:u` | 0/2 ListAudioOuts(Auto): one device, `DeviceOut`. 1/3 OpenAudioOut(Auto): always 48 kHz PCM16. libnx puts the channel count in the upper 16 bits (`0x00020000`). Mono and stereo are kept; other counts are treated as 2. |
| `IAudioOut` | 0 GetAudioOutState, 1 Start, 2 Stop, 3/7 AppendAudioOutBuffer(Auto), 4 RegisterBufferEvent, 5/8 GetReleasedAudioOutBuffers(Auto), 6 Contains, 9 BufferCount, 10 PlayedSampleCount, 11 Flush, 12/13 Set/GetVolume |

The appended buffer is libnx's `AudioOutBuffer` struct: `{ next, buffer, buffer_size, data_size, data_offset }`. The tag is the address of that struct, and it is the value that `GetReleased` returns.

## Timing

A program fills a buffer, appends it and waits for the buffer event before reusing the buffer.
NeXo releases a buffer when its length has passed on the emulated clock:

- `Kernel::ScheduleAt(tick, fn)` runs `fn` under the kernel lock from `UpdateWaits`.
- The idle loops (single-thread and multicore) also wake up for these timers.
- Each buffer starts when the previous one ends (or now, if the queue was empty).
- When a buffer ends it moves to the released list and the event is signalled.

This makes the program run at its real audio rate.
`Stop` changes a generation counter, so a timer that was already scheduled does nothing.

## Sound on the PC

On `Append`, or on `Start` for buffers already queued, the samples go to `Kernel::PushAudio`:

- Mono is duplicated to stereo.
- Volume is applied.
- The app (`main.cpp`) feeds an SDL3 audio stream (48 kHz, stereo, S16).

If more than 0.25 s of sound piles up, it is dropped so latency doesn't grow. `NEXO2_AUDIO=0` turns sound off.
