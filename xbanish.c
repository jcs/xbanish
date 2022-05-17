/*
 * xbanish
 * Copyright (c) 2013-2021 joshua stein <jcs@jcs.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <err.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>
#include <X11/Xutil.h>

void hide_cursor(void);
void show_cursor(void);
void snoop_root(void);
int snoop_xinput(Window);
void snoop_legacy(Window);
void set_alarm(XSyncAlarm *, XSyncTestType);
void usage(char *);
int swallow_error(Display *, XErrorEvent *);
int parse_geometry(const char *s);

/* xinput event type ids to be filled in later */
static int button_press_type = -1;
static int button_release_type = -1;
static int key_press_type = -1;
static int key_release_type = -1;
static int motion_type = -1;
static int device_change_type = -1;
static long last_device_change = -1;

static Display *dpy;
static int hiding = 0, legacy = 0, always_hide = 0, ignore_scroll = 0;
static unsigned timeout = 0;
static unsigned char ignored;
static XSyncCounter idler_counter = 0;
static XSyncAlarm idle_alarm = None;

static int debug = 0;
#define DPRINTF(x) { if (debug) { printf x; } };

static int move = 0, move_x, move_y, move_custom_x, move_custom_y, move_custom_mask;
enum move_types {
	MOVE_NW = 1,
	MOVE_NE,
	MOVE_SW,
	MOVE_SE,
	MOVE_WIN_NW,
	MOVE_WIN_NE,
	MOVE_WIN_SW,
	MOVE_WIN_SE,
	MOVE_CUSTOM,
};

int
main(int argc, char *argv[])
{
	int ch, i;
	XEvent e;
	XSyncAlarmNotifyEvent *alarm_e;
	XGenericEventCookie *cookie;
	XSyncSystemCounter *counters;
	int sync_event, error;
	int major, minor, ncounters;

	struct mod_lookup {
		char *name;
		int mask;
	} mods[] = {
		{"shift", ShiftMask}, {"lock", LockMask},
		{"control", ControlMask}, {"mod1", Mod1Mask},
		{"mod2", Mod2Mask}, {"mod3", Mod3Mask},
		{"mod4", Mod4Mask}, {"mod5", Mod5Mask},
		{"all", -1},
	};

	while ((ch = getopt(argc, argv, "adi:m:t:s")) != -1)
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
			else if (strcmp(optarg, "wnw") == 0)
				move = MOVE_WIN_NW;
			else if (strcmp(optarg, "wne") == 0)
				move = MOVE_WIN_NE;
			else if (strcmp(optarg, "wsw") == 0)
				move = MOVE_WIN_SW;
			else if (strcmp(optarg, "wse") == 0)
				move = MOVE_WIN_SE;
			else if (parse_geometry(optarg))
				move = MOVE_CUSTOM;
			else {
				warnx("invalid '-m' argument");
				usage(argv[0]);
			}
			break;
		case 't':
			timeout = strtoul(optarg, NULL, 0);
			break;
		case 's':
			ignore_scroll = 1;
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

	/* required setup for the xsync alarms used by timeout */
	if (timeout) {
		if (XSyncQueryExtension(dpy, &sync_event, &error) != True)
			errx(1, "no sync extension available");

		XSyncInitialize(dpy, &major, &minor);

		counters = XSyncListSystemCounters(dpy, &ncounters);
		for (i = 0; i < ncounters; i++) {
			if (!strcmp(counters[i].name, "IDLETIME")) {
				idler_counter = counters[i].counter;
				break;
			}
		}
		XSyncFreeSystemCounterList(counters);

		if (!idler_counter)
			errx(1, "no idle counter");
	}

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
				if (ignore_scroll && ((xie->detail >= 4 && xie->detail <= 7) ||
						xie->event_x == xie->event_y))
					break;
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
			if (!timeout ||
			    e.type != (sync_event + XSyncAlarmNotify)) {
				DPRINTF(("unknown event type %d\n", e.type));
				break;
			}

			alarm_e = (XSyncAlarmNotifyEvent *)&e;
			if (alarm_e->alarm == idle_alarm) {
				DPRINTF(("idle counter reached %dms, hiding "
				    "cursor\n",
				    XSyncValueLow32(alarm_e->counter_value)));
				hide_cursor();
			}
		}
	}
}

void
hide_cursor(void)
{
	Window win;
	XWindowAttributes attrs;
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

			XGetWindowAttributes(dpy, win, &attrs);

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
			case MOVE_WIN_NW:
				x = attrs.x;
				y = attrs.y;
				break;
			case MOVE_WIN_NE:
				x = attrs.x + attrs.width;
				y = attrs.y;
				break;
			case MOVE_WIN_SW:
				x = attrs.x;
				y = attrs.x + attrs.height;
				break;
			case MOVE_WIN_SE:
				x = attrs.x + attrs.width;
				y = attrs.x + attrs.height;
				break;
			case MOVE_CUSTOM:
				x = (move_custom_mask & XNegative ? w : 0) + move_custom_x;
				y = (move_custom_mask & YNegative ? h : 0) + move_custom_y;
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

	if (timeout) {
		DPRINTF(("(re)setting timeout of %us\n", timeout));
		set_alarm(&idle_alarm, XSyncPositiveComparison);
	}

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

	if (XQueryTree(dpy, win, &root, &parent, &kids, &nkids) == 0) {
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
set_alarm(XSyncAlarm *alarm, XSyncTestType test)
{
	XSyncAlarmAttributes attr;
	XSyncValue value;
	unsigned int flags;

	XSyncQueryCounter(dpy, idler_counter, &value);

	attr.trigger.counter = idler_counter;
	attr.trigger.test_type = test;
	attr.trigger.value_type = XSyncRelative;
	XSyncIntsToValue(&attr.trigger.wait_value, timeout * 1000,
	    (unsigned long)(timeout * 1000) >> 32);
	XSyncIntToValue(&attr.delta, 0);

	flags = XSyncCACounter | XSyncCATestType | XSyncCAValue | XSyncCADelta;

	if (*alarm)
		XSyncDestroyAlarm(dpy, *alarm);

	*alarm = XSyncCreateAlarm(dpy, flags, &attr);
}

void
usage(char *progname)
{
	fprintf(stderr, "usage: %s [-a] [-d] [-i mod] [-m [w]nw|ne|sw|se|+/-xy] "
	    "[-t seconds] [-s]\n", progname);
	exit(1);
}

int
swallow_error(Display *d, XErrorEvent *e)
{
	if (e->error_code == BadWindow)
		/* no biggie */
		return 0;

	if (e->error_code & FirstExtensionError)
		/* error requesting input on a particular xinput device */
		return 0;

	errx(1, "got X error %d", e->error_code);
}

int
parse_geometry(const char *s)
{
	int x, y;
	unsigned int junk;
	int ret = XParseGeometry(s, &x, &y, &junk, &junk);
	if (((ret & XValue) || (ret & XNegative)) &&
	    ((ret & YValue) || (ret & YNegative))) {
		move_custom_x = x;
		move_custom_y = y;
		move_custom_mask = ret;
		return 1;
	}
	return 0;
}
