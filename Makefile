# splash-drm Makefile
# Self-contained DRM/KMS bootsplash for Linux initrd

CC      ?= gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99 -D_GNU_SOURCE -Wno-unused-function -I./include
LDFLAGS = -lm

# For fully static initrd binary:
# make STATIC=1
ifdef STATIC
    LDFLAGS += -static
endif

DAEMON_SRCS = src/main.c \
              src/drm.c \
              src/render.c \
              src/font.c \
              src/image.c \
              src/elements.c \
              src/pipe.c \
              src/cmd.c \
              src/utils.c

CLI_SRCS = src/splash-cli.c

DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)
CLI_OBJS = $(CLI_SRCS:.c=.o)

DAEMON = splash-drm
CLI = splash-cli

.PHONY: all clean install static

all: $(DAEMON) $(CLI)

$(DAEMON): $(DAEMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLI): $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(DAEMON_OBJS) $(CLI_OBJS) $(DAEMON) $(CLI)

install: $(DAEMON) $(CLI)
	install -D -m 755 $(DAEMON) $(DESTDIR)/usr/bin/$(DAEMON)
	install -D -m 755 $(CLI) $(DESTDIR)/usr/bin/$(CLI)

# Static build target
static:
	$(MAKE) STATIC=1
