# Wayland Desktop Toys — one package: the shared libraries and the toys that
# link them. Libraries build first; each toy also delegates to its libraries
# on its own, so a toy still builds standalone, but building libs up front
# keeps a parallel (make -jN) build correct and free of duplicate work.

# Shared libraries (built here; toys reference them as ../<name> siblings).
# desktop-toys-packaging ships only assets + install.mk, so it is not built.
LIBS := toy-audio ring-menu third_party/lodepng shared

# The interactive toys.
TOYS := paint poingo balloons

.PHONY: all libs clean install uninstall install-user uninstall-user \
        $(LIBS) $(TOYS)

all: $(TOYS)

libs: $(LIBS)

$(LIBS):
	$(MAKE) -C $@

# Toys wait on every library before any of them build.
$(TOYS): libs
	$(MAKE) -C $@

clean:
	@for d in $(LIBS) $(TOYS); do $(MAKE) -C $$d clean; done

# Install/uninstall are toy-only; the toys pull in the shared Desktop Toys
# menu category through desktop-toys-packaging/install.mk themselves.
install install-user uninstall uninstall-user:
	@for d in $(TOYS); do $(MAKE) -C $$d $@; done
