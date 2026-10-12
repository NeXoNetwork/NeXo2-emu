# Audio (audout:u and audren:u)

Two services make sound:

- `audout:u` (`src/core/hle/services/audio.*`): the simple output that libnx's `audoutInitialize` uses, and SDL2 for Switch uses it too.
- `audren:u` (`src/core/hle/services/audren.*`): the audio renderer. Commercial games use it, and so do OpenAL and libnx `audrv`. See [Audio renderer](#audio-renderer-audrenu) below.

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

Latency control: the target is about 60 ms queued in SDL.

- If more or less piles up (the emulation runs a bit fast or slow), `SDL_SetAudioStreamFrequencyRatio` speeds playback up or slows it down by up to 2 %. That is not audible, and there are no cuts.
- Only more than 0.5 s queued (a pause or a load) is dropped.

`NEXO2_AUDIO=0` turns sound off.

## Audio renderer (audren:u)

The program describes:

- **voices**: each plays sample buffers from its own memory;
- **mixes**: intermediate buffers with volumes;
- **sinks**: outputs.

Every 5 ms (240 samples at 48 kHz, or 160 at 32 kHz) the console mixes one chunk and signals an event.
In `RequestUpdate` the program sends its new state and gets back what each voice has consumed.

| Object | Commands |
|---|---|
| `audren:u` | 0 OpenAudioRenderer (0x34-byte parameter: rate, samples, mix buffers, submixes, voices, sinks, effects, …, revision), 1 GetWorkBufferSize, 2/4 GetAudioDeviceService |
| `IAudioRenderer` | 0 GetSampleRate, 1 GetSampleCount, 2 GetMixBufferCount, 3 GetState, 4/10 RequestUpdate(Auto), 5 Start, 6 Stop, 7 QuerySystemEvent, 8/9 rendering time limit |
| `IAudioDevice` | device names (`AudioTvOutput`), volume 1.0, 2 active channels, events |

`RequestUpdate` layout (checked against libnx `audren.h`):

- **Input:** header (0x40, with the size of each section), behaviour, memory pools (0x20 each), voice channels (0x70: 24 volumes, one per mix buffer), voices (0x170), effects (0xC0), splitters, mixes (0x930: volume, buffer count, id, 24×24 matrix, destination), sinks (0x140), performance.
- **Output:** header, pools (new state: RequestAttach → Attached, RequestDetach → Detached), voices (played samples, consumed buffers), effects, sinks (written offset), performance, behaviour (0xB0).
- Counts come from the section sizes in the header, so a different number of voices or revision does not break the parsing.

Voices:

- Each voice has a ring of 4 wave buffers. The program sends `wavebuf_count` buffers from `wavebuf_head`.
- A slot that is not `sent_to_server` yet is new, and the renderer copies it.
- When the renderer finishes a buffer it frees it, moves to the next slot and increments `num_wavebufs_consumed` (cumulative since `is_new`), like libnx `audrv` expects.
- Looping buffers start over.
- Formats:
  - PCM s8 / s16 / s24 / s32 / float;
  - ADPCM (8-byte frames: header + 14 nibbles, 16 coefficients from the voice's extra parameters, history from the buffer context).
- Pitch and sample rate: linear interpolation over a small FIFO of decoded samples.
- The two biquad filters are applied (Q14 coefficients).
- Each voice channel goes to the destination mix's buffers with the volumes of its channel resource.

Mixes and output:

- The final mix (id 0) owns the first buffers; submixes follow in order.
- Submixes are processed farthest-first with their 24×24 matrix and volume.
- **Device sink:** 1, 2 or 6 inputs. 6 inputs are downmixed to stereo (front + 0.707 × centre + 0.707 × rear).
- **Circular-buffer sink:** PCM16 is written into the program's ring.
- 32 kHz is upsampled to 48 kHz. Then the result goes to `Kernel::PushAudio`, like `audout`.
- Effects: only **BufferMixer** (type 1) is done. It adds mix buffers into other buffers of the
  same mix (inputs, outputs and volumes from its parameter: 24 + 24 bytes, 24 floats, a count at
  +0x90), run in the effect's processing order before the mix goes on. Games use it to move the
  sound to the buffers the sink reads (Excitebike: voices on buffers 0-3, mixers into 4/5, sink
  reads 4/5); without it they are silent. Reverb, delay, aux... are still skipped (dry sound).
- `NEXO2_AUDREN_DEBUG=1` also logs the voice channels' volumes, the effects and the sink inputs.

Timing:

- `Start` schedules a timer every chunk on the emulated clock. Each tick mixes, sends the result to the PC and signals the system event.
- If a tick arrives late (the emulated CPU ran in bursts), it mixes the missing chunks, up to 200 ms. So the PC still gets 48,000 samples per second.

Debugging: `NEXO2_AUDREN_DEBUG=1` writes a summary of every `RequestUpdate` to the log (section sizes, voices in use, mixes, sinks).

Tests (`tests/audio_tests.cpp`):

- `Audren_PcmVoiceMixAndBuffersConsumed`:
  - libnx-layout input;
  - PCM voice with left/right volumes;
  - consumed buffers and played samples;
  - silence when there is no data.
- `Audren_AdpcmAndPitch`: ADPCM decoding and a 24 kHz voice interpolated to 48 kHz.
