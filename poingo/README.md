# Poingo

Poingo is a bouncing-ball toy and demo homage: a ball that bounces around your
desktop as a native Wayland overlay.

It is designed to feel simple and direct while still adapting to modern hardware
realities such as variable refresh rates, external displays, and high-latency
audio paths.

## Features
- Physics-driven bouncing-ball animation.
- A pure-Wayland client: its own `wl_surface` + EGL/GLES renderer, native
  `wl_pointer`/`wl_keyboard` input, and `xkbcommon` for the keymap. No SDL, no
  toolkit.
- Predictive audio scheduling for modern output latency.
- Audio through cubeb; the shared mixer/DSP lives in `libtoyaudio`, the pie
  menu in `libringmenu`.
- A built-in `nostalgia mode`.

## Build
Poingo requires a Wayland compositor. Run:

```sh
make          # builds ./bin/poingo
./bin/poingo  # bounce
```

Development packages: `wayland-client`, `wayland-egl`, `egl`, `glesv2`,
`xkbcommon`, and `cubeb` (audio; without it the toy builds and runs silent).
Build and install through the Makefile targets. The project-wide package
checklist is in `../list.todo`.

## Options
```
--mute                 Start with audio muted
--light-color <color>  Light ball color (R,G,B or #RRGGBB)
--dark-color <color>   Dark ball color (R,G,B or #RRGGBB)
--start-size <scale>   Initial ball size (0.25 to 2.0)
--debug                Print FPS to stderr and show the FPS HUD
--help, -h             Show help
```

## Controls
- `M` mute
- `,` / `.` or `<` / `>` speed down / up
- `[` / `]` size down / up
- `C` randomize colors
- `D` toggle debug HUD
- `SPACE` toggle help
- `A` enter `nostalgia mode`
- `P` return to normal Poingo mode

## Audio and latency
Most of Poingo's audio machinery exists for one reason: the ball strikes a wall
at a *known instant*, and a bounce sound that arrives even a couple hundredths of
a second late reads as wrong. A continuous or fire-and-forget sound can absorb
that slack; a sharp impact synced to a visible collision cannot.

So Poingo doesn't just play a sound when the ball bounces — it plays it *ahead of
time*. At startup, and again when the output device changes, it measures the
real latency of the audio path through cubeb (`cubeb_get_min_latency`, then the
running stream's `cubeb_stream_get_latency`). The predictive scheduler queues
each bounce that much earlier, so the sound leaves the speakers exactly as the
ball meets the wall. A small fixed onset-compensation trim accounts for the
sound's own attack, so the *perceived* impact — not the first sample — lands on
the frame.

This is why Poingo depends specifically on cubeb rather than whatever audio path
is nearest to hand: cubeb is one of the few portable backends that reports real
device latency. The shared DSP and mixer live in `libtoyaudio`; cubeb is only
the device layer that makes the prediction accurate. Build without cubeb and the
toy still runs — just silent, with nothing to schedule.

The sibling toys (paint, balloons) carry none of this: a spray hiss is a
continuous stream and a balloon pop is fire-and-forget, so neither has a visual
instant to hit, and a few milliseconds of slack is inaudible.

## Notes
Poingo follows a simple priority order under load:
1. Movement and collision timing.
2. Rotation and visual smoothness.
3. Resolution and secondary presentation details.

The project intentionally keeps the visible behavior simple, even where the
underlying platform adaptation has to be more complex.
