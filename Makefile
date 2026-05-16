# Makefile for splash-drm

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99 -D_GNU_SOURCE -Wno-unused-function -I./include -I/usr/include -I/usr/include/libdrm
LDFLAGS ?= -lm

SRCDIR = src
INCDIR = include
OBJDIR = obj

SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/drm.c \
          $(SRCDIR)/render.c \
          $(SRCDIR)/font.c \
          $(SRCDIR)/image.c \
          $(SRCDIR)/elements.c \
          $(SRCDIR)/socket.c \
          $(SRCDIR)/cmd.c \
          $(SRCDIR)/utils.c

CTL_SOURCES = $(SRCDIR)/splash-ctl.c

CJSON_SOURCES = $(SRCDIR)/cJSON.c

OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES)) $(OBJDIR)/cJSON.o
CTL_OBJECTS = $(OBJDIR)/splash-ctl.o $(OBJDIR)/cJSON.o

TARGET = splash-drm
CTL_TARGET = splash-ctl

.PHONY: all clean static

all: $(TARGET) $(CTL_TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(CTL_TARGET): $(CTL_OBJECTS)
	$(CC) $(CTL_OBJECTS) -o $@

static: CFLAGS += -static
static: LDFLAGS += -static
static: all

clean:
	rm -rf $(OBJDIR) $(TARGET) $(CTL_TARGET)
