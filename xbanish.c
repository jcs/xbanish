/*
 * xbanish
 * Copyright (c) 2013 joshua stein <jcs@jcs.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>

#ifndef nitems
#define nitems(_a) (sizeof((_a)) / sizeof((_a)[0]))
#endif

void snoop(Display *, Window);
void usage(void);
Cursor blank_cursor(Display *, Window);
int ignore_window(Display *, Window);

static int debug = 0;

/* list of windows (by matching name) to ignore and ungrab on */
static char *ignores[] = {
	"xlock",
};

int
main(int argc, char *argv[])
{
	Display *dpy;
	int hiding = 0, ch;
	XEvent e;

	while ((ch = getopt(argc, argv, "d")) != -1)
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

	/* recurse from root window down */
	snoop(dpy, DefaultRootWindow(dpy));

	for (;;) {
		XNextEvent(dpy, &e);

		switch (e.type) {
		case KeyRelease:
			if (debug)
				printf("keystroke %d, %shiding cursor\n",
				    e.xkey.keycode, (hiding ? "already " :
				    ""));

			if (!hiding) {
				Cursor c = blank_cursor(dpy,
				    DefaultRootWindow(dpy));
				hiding = !XGrabPointer(dpy,
				    DefaultRootWindow(dpy), 0,
				    PointerMotionMask | ButtonPressMask |
				    ButtonReleaseMask, GrabModeAsync,
				    GrabModeAsync, None, c, CurrentTime);
			}

			break;

		case ButtonRelease:
		case MotionNotify:
			if (debug)
				printf("mouse moved to %d,%d, %sunhiding "
				    "cursor\n", e.xmotion.x, e.xmotion.y,
				    (hiding ? "" : "already "));

			if (hiding) {
				XUngrabPointer(dpy, CurrentTime);
				hiding = 0;
			}

			break;

		case CreateNotify:
			if (ignore_window(dpy, e.xcreatewindow.window)) {
				if (hiding) {
					XUngrabPointer(dpy, CurrentTime);
					hiding = 0;
				}

				continue;
			}

			if (debug)
				printf("created new window, snooping on it\n");

			/* not sure why snooping directly on the window doesn't
			 * work, so recurse from its parent (probably root) */
			snoop(dpy, e.xcreatewindow.parent);

			break;
		}
	}
}

void
snoop(Display *dpy, Window win)
{
	Window parent, root, *kids;
	XSetWindowAttributes nattrs;
	unsigned int nkids, i;

	/* firefox stops responding to keys when KeyPressMask is used, so
	 * settle for keyreleasemask */
	int type = PointerMotionMask | KeyReleaseMask | Button1MotionMask |
		Button2MotionMask | Button3MotionMask | Button4MotionMask |
		Button5MotionMask | ButtonMotionMask;

	if (XQueryTree(dpy, win, &root, &parent, &kids, &nkids) == FALSE)
		err(1, "can't query window tree\n");

	if (!nkids)
		goto done;

	XSelectInput(dpy, root, type);

	/* listen for newly mapped windows */
	nattrs.event_mask = SubstructureNotifyMask;
	XChangeWindowAttributes(dpy, root, CWEventMask, &nattrs);

	/* recurse */
	for (i = 0; i < nkids; i++) {
		XSelectInput(dpy, kids[i], type);
		snoop(dpy, kids[i]);
	}

done:
	if (kids != NULL)
		XFree(kids); /* hide yo kids */
}

Cursor
blank_cursor(Display *dpy, Window win)
{
	Pixmap mask;
	XColor nocolor;
	Cursor nocursor;

	mask = XCreatePixmap(dpy, win, 1, 1, 1);
	nocolor.pixel = 0;
	nocursor = XCreatePixmapCursor(dpy, mask, mask, &nocolor, &nocolor, 0,
	    0);

	XFreePixmap(dpy, mask);

	return nocursor;
}

void
usage(void)
{
	fprintf(stderr, "usage: xbanish [-d]\n");
	exit(1);
}

int
ignore_window(Display *dpy, Window win)
{
	char *name = NULL;
	XTextProperty text_prop;
	int ret, count, i;
	char **list;

	if (XGetWMName(dpy, win, &text_prop) == 0)
		return 0;

	ret = XmbTextPropertyToTextList(dpy, &text_prop, &list, &count);
	if (ret == Success && list && count > 0) {
		name = strdup(list[0]);
		XFreeStringList(list);
	}
	else if (text_prop.encoding == XA_STRING)
		name = strdup((char*)text_prop.value);

	XFree (text_prop.value);

	if (name == NULL || strlen(name) == 0)
		return 0;

	for (i = 0; i < nitems(ignores); i++)
		if (strncasecmp(name, ignores[i], strlen(ignores[i])) == 0) {
			if (debug)
				printf("ignoring \"%s\" (matches \"%s\")\n",
				    name, ignores[i]);

			return 1;
		}

	return 0;
}
