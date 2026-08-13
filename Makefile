CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -lncursesw

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall test

all: wifi-tui

wifi-tui: wifi_tui.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: wifi-tui
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 wifi-tui $(DESTDIR)$(BINDIR)/wifi-tui

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wifi-tui

clean:
	rm -f wifi-tui

test: wifi-tui
	./tests/smoke.sh
