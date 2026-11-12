# knulli-capture: knulli-screenshot and knulli-record.
#
#   make                     build both, linked against the system's libraries
#   make install DESTDIR=... install to $(DESTDIR)$(PREFIX)/bin
#   make STATIC=1            link x264, opus and vo-aacenc into knulli-record,
#                            to copy onto a Knulli image that lacks them
#
# Cross-compile:
#   make CC=.../aarch64-buildroot-linux-gnu-gcc
#
# Buildroot (see buildroot/) passes CC, CFLAGS, LDFLAGS and VERSION on the
# command line. libpulse-simple and librga are dlopen'ed at run time.
CC      ?= gcc
CFLAGS  ?= -O2
# override: Buildroot passes CFLAGS on the command line.
override CFLAGS += -std=gnu99 -Wall -Wextra
PREFIX  ?= /usr
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

# Shown by -h after "Build: ".
BUILD_DEF := '-DBUILD_TIMESTAMP="knulli-capture $(VERSION)"'

RECORD_SRC := src/knulli-record.c src/kms.c src/yuv.c src/mkv.c src/mp4.c \
              src/rga.c src/sunxi.c
RECORD_HDR := src/kms.h src/yuv.h src/mkv.h src/mp4.h src/rga.h src/sunxi.h

ifeq ($(STATIC),1)
# compat.c: a toolchain newer than the device's glibc would otherwise make the
# static x264/opus need a sqrtf symbol version the device lacks.
RECORD_EXTRA := src/compat.c
RECORD_LIBS  := -Wl,-Bstatic -lx264 -lopus -lvo-aacenc -Wl,-Bdynamic
else
RECORD_LIBS  := -lx264 -lopus -lvo-aacenc
endif

all: knulli-screenshot knulli-record

knulli-screenshot: src/knulli-screenshot.c src/kms.c src/kms.h
	$(CC) $(CFLAGS) $(BUILD_DEF) $(LDFLAGS) -o $@ \
	    src/knulli-screenshot.c src/kms.c -lz

knulli-record: $(RECORD_SRC) $(RECORD_HDR) $(RECORD_EXTRA)
	$(CC) $(CFLAGS) -O3 $(BUILD_DEF) $(LDFLAGS) -o $@ \
	    $(RECORD_SRC) $(RECORD_EXTRA) $(RECORD_LIBS) -lpthread -ldl -lm

install: all
	install -D -m 0755 knulli-screenshot $(DESTDIR)$(PREFIX)/bin/knulli-screenshot
	install -D -m 0755 knulli-record     $(DESTDIR)$(PREFIX)/bin/knulli-record

clean:
	rm -f knulli-screenshot knulli-record

.PHONY: all install clean
