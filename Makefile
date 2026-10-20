# SPDX-License-Identifier: GPL-3.0-or-later
.POSIX:

CC = cc
CFLAGS = -O2 -std=c99 -Wall -Wextra -pedantic
CPPFLAGS =
LDFLAGS =
LDLIBS = -lcurl
# Isocline is built as one translation unit, so helpers it never calls are
# reported as unused. Keep vendored sources unmodified; set empty for compilers
# without this GCC/Clang flag.
ISOCLINE_CFLAGS = -Wno-unused-function
PREFIX = /usr/local

OBJ = hark.o json.o net.o auth.o platform.o line.o store.o session.o yyjson.o isocline.o

all: hark

hark: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

hark.o: hark.c json.h net.h auth.h platform.h line.h store.h session.h
json.o: json.c json.h vendor/yyjson/yyjson.h
net.o: net.c net.h json.h
auth.o: auth.c auth.h net.h json.h platform.h
platform.o: platform.c platform.h json.h
store.o: store.c store.h platform.h json.h
session.o: session.c session.h store.h platform.h json.h
line.o: line.c line.h vendor/isocline/include/isocline.h

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

yyjson.o: vendor/yyjson/yyjson.c vendor/yyjson/yyjson.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c vendor/yyjson/yyjson.c -o $@

ISO_FILES = vendor/isocline/include/isocline.h \
	vendor/isocline/src/attr.c \
	vendor/isocline/src/attr.h \
	vendor/isocline/src/bbcode.c \
	vendor/isocline/src/bbcode.h \
	vendor/isocline/src/bbcode_colors.c \
	vendor/isocline/src/common.c \
	vendor/isocline/src/common.h \
	vendor/isocline/src/completers.c \
	vendor/isocline/src/completions.c \
	vendor/isocline/src/completions.h \
	vendor/isocline/src/editline.c \
	vendor/isocline/src/editline_completion.c \
	vendor/isocline/src/editline_help.c \
	vendor/isocline/src/editline_history.c \
	vendor/isocline/src/env.h \
	vendor/isocline/src/highlight.c \
	vendor/isocline/src/highlight.h \
	vendor/isocline/src/history.c \
	vendor/isocline/src/history.h \
	vendor/isocline/src/isocline.c \
	vendor/isocline/src/stringbuf.c \
	vendor/isocline/src/stringbuf.h \
	vendor/isocline/src/term.c \
	vendor/isocline/src/term.h \
	vendor/isocline/src/term_color.c \
	vendor/isocline/src/tty.c \
	vendor/isocline/src/tty.h \
	vendor/isocline/src/tty_esc.c \
	vendor/isocline/src/undo.c \
	vendor/isocline/src/undo.h \
	vendor/isocline/src/wcwidth.c

isocline.o: $(ISO_FILES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ISOCLINE_CFLAGS) -c vendor/isocline/src/isocline.c -o $@

tests/unit: tests/unit.c json.c json.h platform.c platform.h vendor/yyjson/yyjson.c vendor/yyjson/yyjson.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/unit.c json.c platform.c vendor/yyjson/yyjson.c

test: hark tests/unit
	./tests/unit
	sh tests/smoke.sh ./hark

integration: hark
	python3 tests/integration.py ./hark
	python3 tests/persistence.py ./hark

terminal: hark
	python3 tests/terminal.py ./hark

install: hark
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp hark $(DESTDIR)$(PREFIX)/bin/hark

clean:
	rm -f hark hark.exe $(OBJ) tests/unit tests/unit.exe
