# ===========================================================================
# Makefile for splash-drm
#
#   splash-drm  - the DRM/KMS bootsplash daemon
#   splash-ctl  - the JSON control client
#
# cJSON, stb and qrcodegen are vendored as git submodules in the repository
# root. After cloning, populate them once with `make submodules`.
#
# Common targets:
#   make                normal (dynamic) build of both binaries
#   make static         statically linked build for initramfs (own obj dir)
#   make strip          strip symbols from the built binaries (size)
#   make install        install to $(DESTDIR)$(PREFIX) (sbin + bin)
#   make clean          remove all build artifacts
#
# A normal and a `static` build use separate object directories, so running
# `make static` straight after `make` rebuilds correctly instead of silently
# doing nothing. The two still share the binary name, so run `make clean` if
# you want to flip an existing binary from static back to dynamic.
# ===========================================================================

CC      ?= gcc
STRIP   ?= strip
# User-overridable. The mandatory include/define flags live in CPPFLAGS below
# (via override) so that e.g. `make CFLAGS=-O0` can never drop them.
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -Wno-unused-function
LDFLAGS ?= -lm

SRCDIR    = src
INCDIR    = include
OBJDIR    = obj
CJSONDIR  = cJSON
QRDIR     = qrcodegen/c

# Mandatory preprocessor flags the build cannot compile without (the daemon
# uses accept4/MSG_NOSIGNAL etc. and includes the vendored headers by path).
# The vendored drm/ headers come first and no system libdrm path is added, so
# the self-contained, runtime-libdrm-free build is what actually gets tested.
override CPPFLAGS += -D_GNU_SOURCE -I. -I$(INCDIR) -I$(QRDIR) -Idrm -Idrm/libdrm

# Auto-generated header dependencies, so editing include/*.h rebuilds the
# affected objects (previously a stale, mixed-version binary risk).
DEPFLAGS = -MMD -MP

SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/drm.c \
          $(SRCDIR)/render.c \
          $(SRCDIR)/font.c \
          $(SRCDIR)/image.c \
          $(SRCDIR)/elements.c \
          $(SRCDIR)/anim.c \
          $(SRCDIR)/socket.c \
          $(SRCDIR)/cmd.c \
          $(SRCDIR)/log.c \
          $(SRCDIR)/utils.c \
          $(SRCDIR)/kbd.c \
          $(SRCDIR)/qr.c \
          $(SRCDIR)/usage.c

OBJECTS     = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES)) $(OBJDIR)/cJSON.o $(OBJDIR)/qrcodegen.o
CTL_OBJECTS = $(OBJDIR)/splash-ctl.o $(OBJDIR)/usage.o

TARGET     = splash-drm
CTL_TARGET = splash-ctl

# Install layout (override DESTDIR/PREFIX for packaging).
PREFIX  ?= /usr
DESTDIR ?=
BINDIR   = $(DESTDIR)$(PREFIX)/bin
SBINDIR  = $(DESTDIR)$(PREFIX)/sbin
SHAREDIR = $(DESTDIR)$(PREFIX)/share/splash

.PHONY: all clean static strip install uninstall submodules

all: $(TARGET) $(CTL_TARGET)

# Populate the cJSON / stb / qrcodegen submodules (once, after a fresh clone).
submodules:
	git submodule update --init --recursive

# src/*.c -> $(OBJDIR)/*.o  (covers both the daemon and splash-ctl).
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Vendored cJSON / qrcodegen live outside src/, so they need their own rules.
$(OBJDIR)/cJSON.o: $(CJSONDIR)/cJSON.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJDIR)/qrcodegen.o: $(QRDIR)/qrcodegen.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

# splash-ctl is a plain socket client: no cJSON, no libm needed, but it must
# honour LDFLAGS so `make static` links it statically too.
$(CTL_TARGET): $(CTL_OBJECTS)
	$(CC) $(CTL_OBJECTS) -o $@ $(LDFLAGS)

# Statically linked build for initramfs. Re-invokes make with its own object
# directory so a prior dynamic `make` cannot leave non-static objects behind:
# previously a bare `make static` after `make` was silently a no-op that
# shipped a dynamic binary (make expands prerequisites at parse time, so a
# target-specific OBJDIR alone would not force the rebuild). The sub-make sees
# an empty obj-static/ and recompiles everything with -static.
static:
	+$(MAKE) OBJDIR=obj-static CFLAGS='$(CFLAGS) -static' LDFLAGS='$(LDFLAGS) -static' all

# Strip symbols from the built binaries (kept as a separate target so packagers
# such as OpenWrt, which strip themselves, are never second-guessed).
strip:
	$(STRIP) $(TARGET) $(CTL_TARGET)

install: all
	install -d $(SBINDIR) $(BINDIR) $(SHAREDIR)
	install -m 0755 $(TARGET) $(SBINDIR)/$(TARGET)
	install -m 0755 $(CTL_TARGET) $(BINDIR)/$(CTL_TARGET)
	# Boot-progress companions are sourced (not executed), so 0644.
	install -m 0644 contrib/openwrt-boot-progress-auto.sh $(SHAREDIR)/
	install -m 0644 contrib/openwrt-boot-progress-simple.sh $(SHAREDIR)/
	install -d $(SHAREDIR)/examples
	install -m 0644 examples/simple-boot.json examples/openwrt-boot.json examples/full-featured.json $(SHAREDIR)/examples/

uninstall:
	rm -f $(SBINDIR)/$(TARGET) $(BINDIR)/$(CTL_TARGET)
	rm -f $(SHAREDIR)/openwrt-boot-progress-auto.sh
	rm -f $(SHAREDIR)/openwrt-boot-progress-simple.sh
	rm -f $(SHAREDIR)/examples/simple-boot.json $(SHAREDIR)/examples/openwrt-boot.json $(SHAREDIR)/examples/full-featured.json
	-rmdir $(SHAREDIR)/examples 2>/dev/null
	-rmdir $(SHAREDIR) 2>/dev/null

clean:
	rm -rf obj obj-static $(TARGET) $(CTL_TARGET)

# Header dependencies (skip while cleaning).
ifeq (,$(filter clean,$(MAKECMDGOALS)))
-include $(OBJECTS:.o=.d) $(CTL_OBJECTS:.o=.d)
endif
