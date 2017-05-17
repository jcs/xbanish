# vim:ts=8

VERS	:= 1.5

CC	?= cc
CFLAGS	?= -O2 -Wall -Wunused -Wmissing-prototypes -Wstrict-prototypes

PREFIX	?= /usr/local
BINDIR	?= $(DESTDIR)$(PREFIX)/bin
MANDIR	?= $(DESTDIR)$(PREFIX)/man/man1

INSTALL_PROGRAM ?= install -s
INSTALL_DATA ?= install

X11BASE	?= /usr/X11R6
INCLUDES?= -I$(X11BASE)/include
LDPATH	?= -L$(X11BASE)/lib
LIBS	+= -lX11 -lXfixes -lXi

PROG	= xbanish
OBJS	= xbanish.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(OBJS) $(LDPATH) $(LIBS) -o $@

$(OBJS): *.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: all
	mkdir -p $(BINDIR)
	$(INSTALL_PROGRAM) $(PROG) $(BINDIR)
	mkdir -p $(MANDIR)
	$(INSTALL_DATA) -m 644 xbanish.1 $(MANDIR)/xbanish.1

clean:
	rm -f $(PROG) $(OBJS)

release: all
	@mkdir $(PROG)-${VERS}
	@cp Makefile *.c $(PROG)-$(VERS)/
	@tar -czf ../$(PROG)-$(VERS).tar.gz $(PROG)-$(VERS)
	@rm -rf $(PROG)-$(VERS)/
	@echo "made release ${VERS}"

.PHONY: all install clean
