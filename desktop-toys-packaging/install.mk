# desktop-toys shared packaging — the "Desktop Toys" menu category, its
# directory entry, and its icons, installed identically by every toy.
#
# Include from a toy's Makefile:
#
#   DESKTOP_TOYS_DIR ?= $(HOME)/desktop-toys-packaging
#   include $(DESKTOP_TOYS_DIR)/install.mk
#
# then make the toy's `install` target depend on `desktop-toys-install`
# (honors DESTDIR/PREFIX) and `install-user` on `desktop-toys-install-user`.
# Run your update-desktop-database / gtk-update-icon-cache refresh in the
# toy's target as before.
#
# The uninstall targets remove the shared files; note that other installed
# toys use them too, so only uninstall the category when removing the last
# toy.

# This fragment is usually included before the toy's own `all` target;
# save and restore .DEFAULT_GOAL so our targets never become the default.
DT_SAVED_GOAL := $(.DEFAULT_GOAL)

DT_ICON_SIZES := 16 22 24 32 48 64 128 256 512

.PHONY: desktop-toys-install desktop-toys-install-user \
        desktop-toys-uninstall desktop-toys-uninstall-user

desktop-toys-install:
	install -d "$(DESTDIR)$(PREFIX)/share/desktop-directories"
	install -m 644 "$(DESKTOP_TOYS_DIR)/DesktopToys.directory" "$(DESTDIR)$(PREFIX)/share/desktop-directories/DesktopToys.directory"
	install -d "$(DESTDIR)/etc/xdg/menus/applications-merged"
	install -m 644 "$(DESKTOP_TOYS_DIR)/desktop-toys.menu" "$(DESTDIR)/etc/xdg/menus/applications-merged/desktop-toys.menu"
	install -d "$(DESTDIR)/etc/xdg/menus/rpd-applications-merged"
	install -m 644 "$(DESKTOP_TOYS_DIR)/desktop-toys.menu" "$(DESTDIR)/etc/xdg/menus/rpd-applications-merged/desktop-toys.menu"
	for size in $(DT_ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 "$(DESKTOP_TOYS_DIR)/desktop-toys-icon-$${size}.png" "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/desktop-toys.png"; \
	done

desktop-toys-install-user:
	install -d "$(HOME)/.local/share/desktop-directories"
	install -m 644 "$(DESKTOP_TOYS_DIR)/DesktopToys.directory" "$(HOME)/.local/share/desktop-directories/DesktopToys.directory"
	install -d "$(HOME)/.config/menus/applications-merged"
	install -m 644 "$(DESKTOP_TOYS_DIR)/desktop-toys.menu" "$(HOME)/.config/menus/applications-merged/desktop-toys.menu"
	install -d "$(HOME)/.config/menus/rpd-applications-merged"
	install -m 644 "$(DESKTOP_TOYS_DIR)/desktop-toys.menu" "$(HOME)/.config/menus/rpd-applications-merged/desktop-toys.menu"
	for size in $(DT_ICON_SIZES); do \
		install -d "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 "$(DESKTOP_TOYS_DIR)/desktop-toys-icon-$${size}.png" "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/desktop-toys.png"; \
	done

desktop-toys-uninstall:
	$(RM) "$(DESTDIR)$(PREFIX)/share/desktop-directories/DesktopToys.directory"
	$(RM) "$(DESTDIR)/etc/xdg/menus/applications-merged/desktop-toys.menu"
	$(RM) "$(DESTDIR)/etc/xdg/menus/rpd-applications-merged/desktop-toys.menu"
	for size in $(DT_ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/desktop-toys.png"; \
	done

desktop-toys-uninstall-user:
	$(RM) "$(HOME)/.local/share/desktop-directories/DesktopToys.directory"
	$(RM) "$(HOME)/.config/menus/applications-merged/desktop-toys.menu"
	$(RM) "$(HOME)/.config/menus/rpd-applications-merged/desktop-toys.menu"
	for size in $(DT_ICON_SIZES); do \
		$(RM) "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/desktop-toys.png"; \
	done

.DEFAULT_GOAL := $(DT_SAVED_GOAL)
