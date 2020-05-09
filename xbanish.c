/*
 * xbanish
 * Copyright (c) 2013-2015 joshua stein <jcs@jcs.org>
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
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>

void hide_cursor(void);
void show_cursor(void);
void snoop_root(void);
int snoop_xinput(Window);
void snoop_legacy(Window);
void usage(char *);
int swallow_error(Display *, XErrorEvent *);

/* xinput event type ids to be filled in later */
static int button_press_type = -1;
static int button_release_type = -1;
static int key_press_type = -1;
static int key_release_type = -1;
static int motion_type = -1;
static int device_change_type = -1;
static long last_device_change = -1;

static Display *dpy;
static int hiding = 0, legacy = 0, always_hide = 0;
static unsigned char ignored;

static int debug = 0;
#define DPRINTF(x) { if (debug) { printf x; } };

static int move = 0, move_x, move_y;
enum move_types {
	MOVE_NW = 1,
	MOVE_NE,
	MOVE_SW,
	MOVE_SE,
};

int
main(int argc, char *argv[])
{
	int ch, i;
	XEvent e;
	XGenericEventCookie *cookie;

	struct mod_lookup {
		char *name;
		int mask;
	} mods[] = {
		{"shift", ShiftMask}, {"lock", LockMask},
		{"control", ControlMask}, {"mod1", Mod1Mask},
		{"mod2", Mod2Mask}, {"mod3", Mod3Mask},
		{"mod4", Mod4Mask}, {"mod5", Mod5Mask}
	};

	while ((ch = getopt(argc, argv, "adi:m:")) != -1)
		switch (ch) {
		case 'a':
			always_hide = 1;
			break;
		case 'd':
			debug = 1;
			break;
		case 'i':
			for (i = 0;
			    i < sizeof(mods) / sizeof(struct mod_lookup); i++)
				if (strcasecmp(optarg, mods[i].name) == 0)
					ignored |= mods[i].mask;

			break;
		case 'm':
			if (strcmp(optarg, "nw") == 0)
				move = MOVE_NW;
			else if (strcmp(optarg, "ne") == 0)
				move = MOVE_NE;
			else if (strcmp(optarg, "sw") == 0)
				move = MOVE_SW;
			else if (strcmp(optarg, "se") == 0)
				move = MOVE_SE;
			else {
				warnx("invalid '-m' argument");
				usage(argv[0]);
			}
			break;
		default:
			usage(argv[0]);
		}

	argc -= optind;
	argv += optind;

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

#ifdef __OpenBSD__
	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");
#endif

	XSetErrorHandler(swallow_error);

	snoop_root();

	if (always_hide)
		hide_cursor();

	for (;;) {
		cookie = &e.xcookie;
		XNextEvent(dpy, &e);

		int etype = e.type;
		if (e.type == motion_type)
			etype = MotionNotify;
		else if (e.type == key_press_type ||
		    e.type == key_release_type)
			etype = KeyRelease;
		else if (e.type == button_press_type ||
		    e.type == button_release_type)
			etype = ButtonRelease;
		else if (e.type == device_change_type) {
			XDevicePresenceNotifyEvent *xdpe =
			    (XDevicePresenceNotifyEvent *)&e;
			if (last_device_change == xdpe->serial)
				continue;
			snoop_root();
			last_device_change = xdpe->serial;
			continue;
		}

		switch (etype) {
		case KeyRelease:
			if (ignored) {
				unsigned int state = 0;

				/* masks are only set on key release, if
				 * ignore is set we must throw out non-release
				 * events here */
				if (e.type == key_press_type) {
					break;
				}

				/* extract modifier state */
				if (e.type == key_release_type) {
					/* xinput device event */
					XDeviceKeyEvent *key =
					    (XDeviceKeyEvent *) &e;
					state = key->state;
				} else if (e.type == KeyRelease) {
					/* legacy event */
					state = e.xkey.state;
				}

				if (state & ignored) {
					DPRINTF(("ignoring key %d\n", state));
					break;
				}
			}

			hide_cursor();
			break;

		case ButtonRelease:
		case MotionNotify:
			if (!always_hide)
				show_cursor();
			break;

		case CreateNotify:
			if (legacy) {
				DPRINTF(("new window, snooping on it\n"));

				/* not sure why snooping directly on the window
				 * doesn't work, so snoop on all windows from
				 * its parent (probably root) */
				snoop_legacy(e.xcreatewindow.parent);
			}
			break;

		case GenericEvent:
			/* xi2 raw event */
			XGetEventData(dpy, cookie);
			XIDeviceEvent *xie = (XIDeviceEvent *)cookie->data;

			switch (xie->evtype) {
			case XI_RawMotion:
			case XI_RawButtonPress:
				if (!always_hide)
					show_cursor();
				break;

			case XI_RawButtonRelease:
				break;

			default:
				DPRINTF(("unknown XI event type %d\n",
				    xie->evtype));
			}

			XFreeEventData(dpy, cookie);
			break;

		default:
			DPRINTF(("unknown event type %d\n", e.type));
		}
	}
}

void
hide_cursor(void)
{
	Window win;
	int x, y, h, w, junk;
	unsigned int ujunk;

	DPRINTF(("keystroke, %shiding cursor\n", (hiding ? "already " : "")));

	if (hiding)
		return;

	if (move) {
		if (XQueryPointer(dpy, DefaultRootWindow(dpy),
		    &win, &win, &x, &y, &junk, &junk, &ujunk)) {
			move_x = x;
			move_y = y;

			h = XHeightOfScreen(DefaultScreenOfDisplay(dpy));
			w = XWidthOfScreen(DefaultScreenOfDisplay(dpy));

			switch (move) {
			case MOVE_NW:
				x = 0;
				y = 0;
				break;
			case MOVE_NE:
				x = w;
				y = 0;
				break;
			case MOVE_SW:
				x = 0;
				y = h;
				break;
			case MOVE_SE:
				x = w;
				y = h;
				break;
			}

			XWarpPointer(dpy, None, DefaultRootWindow(dpy),
			    0, 0, 0, 0, x, y);
		} else {
			move_x = -1;
			move_y = -1;
			warn("failed finding cursor coordinates");
		}
	}

	XFixesHideCursor(dpy, DefaultRootWindow(dpy));
	hiding = 1;
}

void
show_cursor(void)
{
	DPRINTF(("mouse moved, %sunhiding cursor\n",
	    (hiding ? "" : "already ")));

	if (!hiding)
		return;

	if (move && move_x != -1 && move_y != -1)
		XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0,
		    move_x, move_y);

	XFixesShowCursor(dpy, DefaultRootWindow(dpy));
	hiding = 0;
}

void
snoop_root(void)
{
	if (snoop_xinput(DefaultRootWindow(dpy)) == 0) {
		DPRINTF(("no XInput devices found, using legacy snooping"));
		legacy = 1;
		snoop_legacy(DefaultRootWindow(dpy));
	}
}

int
snoop_xinput(Window win)
{
	int opcode, event, error, numdevs, i, j;
	int major, minor, rc, rawmotion = 0;
	int ev = 0;
	unsigned char mask[(XI_LASTEVENT + 7)/8];
	XDeviceInfo *devinfo = NULL;
	XInputClassInfo *ici;
	XDevice *device;
	XIEventMask evmasks[1];
	XEventClass class_presence;

	if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error)) {
		DPRINTF(("XInput extension not available"));
		return 0;
	}

	/*
	 * If we support xinput 2, use that for raw motion and button events to
	 * get pointer data when the cursor is over a Chromium window.  We
	 * could also use this to get raw key input and avoid the other XInput
	 * stuff, but we may need to be able to examine the key value later to
	 * filter out ignored keys.
	 */
	major = minor = 2;
	rc = XIQueryVersion(dpy, &major, &minor);
	if (rc != BadRequest) {
		memset(mask, 0, sizeof(mask));

		XISetMask(mask, XI_RawMotion);
		XISetMask(mask, XI_RawButtonPress);
		evmasks[0].deviceid = XIAllMasterDevices;
		evmasks[0].mask_len = sizeof(mask);
		evmasks[0].mask = mask;

		XISelectEvents(dpy, win, evmasks, 1);
		XFlush(dpy);

		rawmotion = 1;

		DPRINTF(("using xinput2 raw motion events\n"));
	}

	devinfo = XListInputDevices(dpy, &numdevs);
	XEventClass event_list[numdevs * 2];
	for (i = 0; i < numdevs; i++) {
		if (devinfo[i].use != IsXExtensionKeyboard &&
		    devinfo[i].use != IsXExtensionPointer)
			continue;

		if (!(device = XOpenDevice(dpy, devinfo[i].id)))
			break;

		for (ici = device->classes, j = 0; j < devinfo[i].num_classes;
		ici++, j++) {
			switch (ici->input_class) {
			case KeyClass:
				DPRINTF(("attaching to keyboard device %s "
				    "(use %d)\n", devinfo[i].name,
				    devinfo[i].use));

				DeviceKeyPress(device, key_press_type,
				    event_list[ev]); ev++;
				DeviceKeyRelease(device, key_release_type,
				    event_list[ev]); ev++;
				break;

			case ButtonClass:
				if (rawmotion)
					continue;

				DPRINTF(("attaching to buttoned device %s "
				    "(use %d)\n", devinfo[i].name,
				    devinfo[i].use));

				DeviceButtonPress(device, button_press_type,
				    event_list[ev]); ev++;
				DeviceButtonRelease(device,
				    button_release_type, event_list[ev]); ev++;
				break;

			case ValuatorClass:
				if (rawmotion)
					continue;

				DPRINTF(("attaching to pointing device %s "
				    "(use %d)\n", devinfo[i].name,
				    devinfo[i].use));

				DeviceMotionNotify(device, motion_type,
				    event_list[ev]); ev++;
				break;
			}
		}

		XCloseDevice(dpy, device);

		if (XSelectExtensionEvent(dpy, win, event_list, ev)) {
			warn("error selecting extension events");
			ev = 0;
			goto done;
		}
	}

	DevicePresence(dpy, device_change_type, class_presence);
	if (XSelectExtensionEvent(dpy, win, &class_presence, 1)) {
		warn("error selecting extension events");
		ev = 0;
		goto done;
	}

done:
	if (devinfo != NULL)
	   XFreeDeviceList(devinfo);

	return ev;
}

void
snoop_legacy(Window win)
{
	Window parent, root, *kids = NULL;
	XSetWindowAttributes sattrs;
	unsigned int nkids = 0, i;

	/*
	 * Firefox stops responding to keys when KeyPressMask is used, so
	 * settle for KeyReleaseMask
	 */
	int type = PointerMotionMask | KeyReleaseMask | Button1MotionMask |
		Button2MotionMask | Button3MotionMask | Button4MotionMask |
		Button5MotionMask | ButtonMotionMask;

	if (XQueryTree(dpy, win, &root, &parent, &kids, &nkids) == FALSE) {
		warn("can't query window tree\n");
		goto done;
	}

	XSelectInput(dpy, root, type);

	/* listen for newly mapped windows */
	sattrs.event_mask = SubstructureNotifyMask;
	XChangeWindowAttributes(dpy, root, CWEventMask, &sattrs);

	for (i = 0; i < nkids; i++) {
		XSelectInput(dpy, kids[i], type);
		snoop_legacy(kids[i]);
	}

done:
	if (kids != NULL)
		XFree(kids); /* hide yo kids */
}

void
usage(char *progname)
{
	fprintf(stderr, "usage: %s [-a] [-d] [-i mod] [-m nw|ne|sw|se]\n",
	    progname);
	exit(1);
}

int
swallow_error(Display *d, XErrorEvent *e)
{
	if (e->error_code == BadWindow)
		/* no biggie */
		return 0;
	else if (e->error_code & FirstExtensionError)
		/* error requesting input on a particular xinput device */
		return 0;
	else
		errx(1, "got X error %d", e->error_code);
}
