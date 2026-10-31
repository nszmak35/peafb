# peafb - Peaclock-like Femboys: boykisser ASCII art in your terminal
#
#   make              build ./peafb
#   make install      install to $(PREFIX)/bin  (default PREFIX=/usr/local)
#   make uninstall    remove it again
#   make clean        remove build output
#
# DESTDIR and PREFIX are honoured, e.g.:  make install PREFIX=$$HOME/.local

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lm

TARGET  := peafb
SRC     := src/peafb.c src/ascii_schemes.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) -std=c99 $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
