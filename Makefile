# wifi-tui for Linux (NetworkManager).
# The FreeBSD build has its own Makefile in FreeBSD/.

PROG = wifi-tui
SRCS = wifi_tui.c
MAN1 = wifi-tui.1

CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -lncursesw

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
MAN1DIR ?= $(MANDIR)/man1

.PHONY: all clean install uninstall test

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(PROG) $(SRCS) $(LDFLAGS) $(LDLIBS)

install: $(PROG)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	install -d $(DESTDIR)$(MAN1DIR)
	install -m 0644 $(MAN1) $(DESTDIR)$(MAN1DIR)/$(MAN1)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)
	rm -f $(DESTDIR)$(MAN1DIR)/$(MAN1)

clean:
	rm -f $(PROG)

test: $(PROG)
	sh ./tests/smoke.sh
