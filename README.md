# Wayland Desktop Toys

The interactive desktop toys, native Wayland on Raspberry Pi OS. Each is its
own git repo; this directory just gathers them and fans out `make` targets.
(The non-interactive screensavers live in `~/wayland-screensavers`.)

## Included Toys

1. **Paint** (`paint`): transparent fullscreen paint overlay with two
   tools on one canvas — SPLAT throws flat glossy enamel blobs, SPRAY is your own graffiti-ready spraycan!
2. **Poingo** (`poingo`): a ball bounces around your desktop with
   a crashing boing sound.  Retro memories!
3. **Balloons** (`balloons`): colorful balloons drift up the desktop,
   with occasional breezes.  Sometimes, the storms come. Pop them if you want to!

Right click for the menu.  You can "GHOST" any of them, they will spookily get on with it in the background while you work.  Click on the big icon in the upper right of your screen to play with them again!

## Build and Installation

```bash
make -j4            # build all toys
make install-user   # install locally into ~/.local
sudo make install   # install system-wide
make clean
```
