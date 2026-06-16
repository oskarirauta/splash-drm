# ===========================================================================
# Makefile for splash-drm
#
#   splash-drm  - the DRM/KMS bootsplash daemon
#   splash-ctl  - the JSON control client
#
# cJSON and stb are vendored as git submodules in the repository root:
#
#   cJSON/cJSON.c   cJSON/cJSON.h
#   stb/stb_image.h   stb/stb_truetype.h
#
# After cloning, populate them once with `make submodules` (or
# `git submodule update --init`). The `-I.` flag lets the sources include
# them as "cJSON/cJSON.h" and "stb/stb_image.h".
# ===========================================================================

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -D_GNU_SOURCE -Wno-unused-function \
           -I. -I$(INCDIR) -I$(QRDIR) -I/usr/include/libdrm
LDFLAGS ?= -lm

SRCDIR    = src
INCDIR    = include
OBJDIR    = obj
CJSONDIR  = cJSON
QRDIR     = qrcodegen/c

# Daemon sources (src/); the vendored cJSON and qrcodegen units are built by their own rules.
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/drm.c \
          $(SRCDIR)/render.c \
          $(SRCDIR)/font.c \
          $(SRCDIR)/image.c \
          $(SRCDIR)/elements.c \
          $(SRCDIR)/anim.c \
          $(SRCDIR)/socket.c \
          $(SRCDIR)/cmd.c \
          $(SRCDIR)/utils.c \
          $(SRCDIR)/kbd.c \
          $(SRCDIR)/qr.c

OBJECTS     = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES)) $(OBJDIR)/cJSON.o $(OBJDIR)/qrcodegen.o
CTL_OBJECTS = $(OBJDIR)/splash-ctl.o

TARGET     = splash-drm
CTL_TARGET = splash-ctl

.PHONY: all clean static submodules

all: $(TARGET) $(CTL_TARGET)

# Populate the cJSON / stb submodules (needed once after a fresh clone).
submodules:
	git submodule update --init --recursive

$(OBJDIR):
	mkdir -p $(OBJDIR)

# src/*.c -> obj/*.o  (covers both the daemon and splash-ctl).
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored cJSON lives outside src/, so it needs a rule of its own.
$(OBJDIR)/cJSON.o: $(CJSONDIR)/cJSON.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored qrcodegen likewise lives outside src/.
$(OBJDIR)/qrcodegen.o: $(QRDIR)/qrcodegen.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

# splash-ctl is a plain socket client: no cJSON, no libm.
$(CTL_TARGET): $(CTL_OBJECTS)
	$(CC) $(CTL_OBJECTS) -o $@

# Statically linked build for initramfs.
static: CFLAGS  += -static
static: LDFLAGS += -static
static: all

clean:
	rm -rf $(OBJDIR) $(TARGET) $(CTL_TARGET)
