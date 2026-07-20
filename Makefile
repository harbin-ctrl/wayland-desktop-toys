# Wayland Desktop Toys — one package: the shared libraries and the toys that
# link them. Libraries build first; each toy also delegates to its libraries
# on its own, so a toy still builds standalone, but building libs up front
# keeps a parallel (make -jN) build correct and free of duplicate work.

# Shared libraries (built here; toys reference them as ../<name> siblings).
# desktop-toys-packaging ships only assets + install.mk, so it is not built.
LIBS := toy-audio ring-menu third_party/lodepng shared

# The interactive toys.
TOYS := paint poingo balloons

LINT_SOURCES := \
	toy-audio/toy_audio.c \
	ring-menu/ringmenu.c \
	shared/ghost_icon.c \
	paint/src/paint.c \
	poingo/src/poingo.c \
	balloons/balloon_gen.c balloons/thunder_synth.c balloons/audio.c balloons/balloons.c
LINT_INCLUDES := -Itoy-audio -Iring-menu -Ishared -Ipaint -Ipoingo -Iballoons -Ithird_party/lodepng
TIDY_SOURCES := $(addprefix $(CURDIR)/,$(LINT_SOURCES))

.PHONY: all libs clean install uninstall install-user uninstall-user \
	$(LIBS) $(TOYS) lint cppcheck analyzer tidy compile_commands.json

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

lint:
	$(MAKE) cppcheck
	$(MAKE) analyzer
	$(MAKE) tidy

cppcheck:
	cppcheck -j2 --quiet --enable=warning,performance,portability \
		--error-exitcode=1 --suppress=missingIncludeSystem \
		--suppress=normalCheckLevelMaxBranches \
		--std=c11 --language=c $(LINT_INCLUDES) $(LINT_SOURCES)

analyzer:
	$(MAKE) clean
	# Static analyzer findings are printed for review; the compiler/build still gates this target.
	scan-build --exclude third_party --use-cc=clang --keep-going $(MAKE) -j1 all

compile_commands.json:
	$(MAKE) clean
	bear --output $@ -- $(MAKE) -j1 all

tidy: compile_commands.json
	@status=0; for src in $(TIDY_SOURCES); do \
		clang-tidy -p . "$$src" \
			--checks='-*,clang-analyzer-core.NullDereference,clang-analyzer-core.DivideZero,clang-analyzer-core.UndefinedBinaryOperatorResult,clang-analyzer-unix.Malloc' \
			--extra-arg=-I$(CURDIR)/toy-audio --extra-arg=-I$(CURDIR)/ring-menu \
			--extra-arg=-I$(CURDIR)/shared --extra-arg=-I$(CURDIR)/balloons --quiet || status=1; \
	done; exit $$status
