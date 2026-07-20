# toy-audio

The shared audio core of the desktop toys (Poingo, balloons): the thunder
bounce synthesizer in both styles (Poingo rumble and the nostalgia
struck-sheet boom with its hall reverb), the milliseconds-scale Schroeder
"feel" micro-reverb, peak normalization, detuned two-layer sample pairs, and
the 16-voice snapshot mixer with master volume/mute, soft clip rescue, a
quit-squelch fade, and the "let it ring out before quitting" quiet tracker.

Extracted after balloons had lifted Poingo's subsystem wholesale; both toys
now build against this single copy. One `.c`/`.h` pair, no dependencies
beyond libm and libc.

## What stays in the toy

- **The audio device.** Poingo plays through SDL audio, balloons through
  cubeb. The toy owns its stream and calls `toy_mixer_render()` from its
  output callback.
- **The lock.** The mixer does no locking; wrap every mixer call (render,
  start_voice, volume, mute, fade, quiet) in whatever guards your callback —
  `SDL_LockAudio`, a pthread mutex, etc.
- **The sounds.** The library ships only the bounce (both toys play it);
  pops, thumps, whooshes and friends are toy-side recipes built from the
  library's idioms (`toy_audio_micro_reverb`, `toy_audio_normalize`,
  `TOY_AUDIO_LAYER_DETUNE`) and played through `toy_mixer_start_voice`.
- **Synthesis threading.** Generation takes tens of milliseconds; run it off
  the render thread (balloons uses a worker) and publish finished buffers
  under the audio lock.

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

// in the device output callback (mono float 48 kHz)
lock(); toy_mixer_render(&mixer, out, nframes, false); unlock();

// on a hit
if (bounce.dirty) toy_audio_generate_bounce_pair(&bounce, size_scale, style);
lock(); toy_mixer_start_voice(&mixer, &bounce, volume01); unlock();
```
