# wifi-tui for FreeBSD.
#
# This Makefile is written to work with both FreeBSD's make(1) (bmake) and GNU
# make, so it deliberately avoids:
#   * $< and $^, which BSD make only defines inside suffix rules
#   * .if / ifeq conditionals, which the two makes spell differently
#   * include of bsd.prog.mk, so that DESTDIR/PREFIX behave the usual way
#
# Common overrides:
#   make PREFIX=/usr                 install somewhere other than /usr/local
#   make DESTDIR=/tmp/stage install  stage into a directory
#   make CPPFLAGS=-I/usr/local/include/ncurses LDFLAGS=-L/usr/local/lib \
#        LDLIBS=-lncursesw           build against devel/ncurses from ports

PROG=		wifi-tui
SRCS=		wifi_tui.c
MAN1=		wifi-tui.1

CC?=		cc
CFLAGS?=	-O2 -pipe
CFLAGS+=	-std=c11 -Wall -Wextra -Wpedantic

# FreeBSD's base ncurses is wide-character capable and installs libncursesw.
LDLIBS+=	-lncursesw

PREFIX?=	/usr/local
BINDIR?=	$(PREFIX)/bin
MANDIR?=	$(PREFIX)/share/man
MAN1DIR?=	$(MANDIR)/man1

INSTALL?=		install
INSTALL_DIR?=		$(INSTALL) -d -m 0755
INSTALL_PROGRAM?=	$(INSTALL) -s -m 0555
INSTALL_MAN?=		$(INSTALL) -m 0444

.PHONY: all clean install uninstall test

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(PROG) $(SRCS) $(LDFLAGS) $(LDLIBS)

install: $(PROG)
	$(INSTALL_DIR) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	$(INSTALL_DIR) $(DESTDIR)$(MAN1DIR)
	$(INSTALL_MAN) $(MAN1) $(DESTDIR)$(MAN1DIR)/$(MAN1)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)
	rm -f $(DESTDIR)$(MAN1DIR)/$(MAN1)

clean:
	rm -f $(PROG)

test: $(PROG)
	sh ./tests/smoke.sh
