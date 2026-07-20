# toy-audio

The shared audio core of the desktop toys (Poingo, Balloons, and Paint): the
native PipeWire playback and latency layer, the thunder
bounce synthesizer in both styles (Poingo rumble and the nostalgia
struck-sheet boom with its hall reverb), the milliseconds-scale Schroeder
"feel" micro-reverb, peak normalization, detuned two-layer sample pairs, and
the 32-voice snapshot mixer with master volume/mute, soft clip rescue, a
quit-squelch fade, and the "let it ring out before quitting" quiet tracker.

Extracted after Balloons had lifted Poingo's subsystem wholesale; all three
toys now build against this single audio implementation. The DSP depends only
on libm and libc; the device layer depends on PipeWire.

## What stays in the toy

- **The lock.** The mixer does no locking. Claim and commit/cancel a voice
  under the callback mutex, but copy its snapshot between those operations
  without the mutex. A claimed voice is marked preparing and skipped by the
  renderer, so the callback never waits for the large copy.
- **The sounds.** The library ships only the bounce (both toys play it);
  pops, thumps, whooshes and friends are toy-side recipes built from the
  library's idioms (`toy_audio_micro_reverb`, `toy_audio_normalize`,
  `TOY_AUDIO_LAYER_DETUNE`) and played through the claim/copy/commit API.
- **Synthesis threading.** Generation takes tens of milliseconds; run it off
  the render thread (balloons uses a worker) and publish finished buffers
  under the audio lock.

## Shared device layer

`toy_audio_stream_start()` creates a native PipeWire stream with strict
floating-point MONO or `FL,FR` negotiation. It owns the PipeWire loop, mapped
buffers, stream lifecycle, and timing query. The toy supplies a render
callback and keeps ownership of its DSP state and synchronization.

The process callback performs no application-side allocation. Latency is
published atomically and includes queued frames, converter/resampler
buffering, and the remaining graph/device delay.

## Build and link

`make` builds `libtoyaudio.a`:

```make
TOYAUDIO_DIR ?= $(HOME)/toy-audio
CFLAGS += -I$(TOYAUDIO_DIR)
$(TOYAUDIO_DIR)/libtoyaudio.a:
	$(MAKE) -C $(TOYAUDIO_DIR)
```

## Sketch

```c
static ToyMixer mixer;
static ToySamplePair bounce;

// startup
toy_mixer_init(&mixer, TOY_AUDIO_DEFAULT_VOLUME);
toy_sample_pair_alloc(&bounce, toy_audio_bounce_pair_length());

// render callback supplied to ToyAudioStreamConfig
lock(); toy_mixer_render_stereo(&mixer, out, nframes, false); unlock();

// on a hit
if (bounce.dirty) toy_audio_generate_bounce_pair(&bounce, size_scale, style);
ToyVoiceClaim claim;
lock();
bool claimed = toy_mixer_claim_voice(&mixer, bounce.length, &claim);
unlock();
if (claimed) {
    bool copied = toy_mixer_copy_claimed_voice(&mixer, claim, &bounce);
    lock();
    if (!copied || !toy_mixer_commit_voice(
            &mixer, claim, bounce.length, volume01, 1.0f, 1.0f)) {
        toy_mixer_cancel_voice(&mixer, claim);
    }
    unlock();
}
```
