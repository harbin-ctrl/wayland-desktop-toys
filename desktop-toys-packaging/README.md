# desktop-toys-packaging

The shared desktop packaging of the toys (spray, splat, Poingo, balloons):
the "Desktop Toys" application-menu category. Every toy used to carry its own
byte-identical copies of these files; now they live once, here.

- `desktop-toys.menu` — merged-menu file placing apps with the `DesktopToy`
  category into a Desktop Toys submenu (installed to both
  `applications-merged` and Raspberry Pi's `rpd-applications-merged`).
- `DesktopToys.directory` — the submenu's name and icon.
- `desktop-toys-icon-*.png` (+ `desktop-toys-icon.svg` source) — the category
  icon at the hicolor sizes.
- `install.mk` — include-able make fragment with
  `desktop-toys-install[-user]` / `desktop-toys-uninstall[-user]` targets;
  see its header comment for usage.

Each toy still owns its own `.desktop` entry and app icons; only the shared
category lives here.
