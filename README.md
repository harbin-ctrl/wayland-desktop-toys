# Wayland Desktop Toys

The interactive desktop toys, native Wayland on Raspberry Pi OS. Each is its
own git repo; this directory just gathers them and fans out `make` targets.
(The non-interactive screensavers live in `~/wayland-screensavers`.)

## Included Toys

1. **Paint** (`paint`): transparent fullscreen paint overlay with two
   tools on one canvas — SPLAT throws flat glossy enamel blobs, SPRAY mists
   translucent mixing paint that drips when laid on thick. Supersedes the old
   spray-wayland and splat-wayland.
2. **Poingo** (`poingo`): a ball bounces around your desktop with
   procedurally synthesized thunder.
3. **Balloons** (`balloons`): colorful balloons drift up the desktop,
   with occasional breezes that blow them sideways.

They share the library checkouts at `$HOME`: `ring-menu` (circular
menu + color field), `toy-audio` (mixer + bounce synthesizer), and
`desktop-toys-packaging` (the Desktop Toys menu category and install.mk).

## Build and Installation

```bash
make -j4            # build all toys
make install-user   # install locally into ~/.local
sudo make install   # install system-wide
make clean
```
