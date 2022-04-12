# vim:ts=8

CC	?= cc
CFLAGS	?= -O2
CFLAGS	+= -std=c99 -Wall -Wunused -Wmissing-prototypes -Wstrict-prototypes -Wunused

PREFIX	?= /usr/local
BINDIR	?= $(PREFIX)/bin
MANDIR	?= $(PREFIX)/man/man1

INSTALL_PROGRAM ?= install -s
INSTALL_DATA ?= install

X11BASE	?= /usr/X11R6
INCLUDES?= -I$(X11BASE)/include
LDPATH	?= -L$(X11BASE)/lib
LIBS	+= -lX11 -lXfixes -lXi -lXext

PROG	= xbanish
OBJS	= xbanish.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(OBJS) $(LDPATH) $(LIBS) -o $@

$(OBJS): *.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(PROG) $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)
	$(INSTALL_DATA) -m 644 xbanish.1 $(DESTDIR)$(MANDIR)/xbanish.1

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all install clean
