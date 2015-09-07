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
int snoop_xinput(Window);
void snoop_legacy(Window);
void usage(void);
int swallow_error(Display *, XErrorEvent *);

/* xinput event type ids to be filled in later */
static int button_press_type = -1;
static int button_release_type = -1;
static int key_release_type = -1;
static int motion_type = -1;

extern char *__progname;

static Display *dpy;
static int debug = 0, hiding = 0, legacy = 0;
static unsigned char ignored;

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

	while ((ch = getopt(argc, argv, "di:")) != -1)
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'i':
			for (i = 0;
			    i < sizeof(mods) / sizeof(struct mod_lookup); i++)
				if (strcasecmp(optarg, mods[i].name) == 0)
					ignored |= mods[i].mask;

			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

	XSetErrorHandler(swallow_error);

	if (snoop_xinput(DefaultRootWindow(dpy)) == 0) {
		if (debug)
			warn("no XInput devices found, using legacy "
			    "snooping");

		legacy = 1;
		snoop_legacy(DefaultRootWindow(dpy));
	}

	for (;;) {
		cookie = &e.xcookie;
		XNextEvent(dpy, &e);

		int etype = e.type;
		unsigned int keycode;
		unsigned int state;

		if (e.type == motion_type)
			etype = MotionNotify;
		else if (e.type == key_release_type)
			etype = KeyRelease;
		else if (e.type == button_press_type ||
		    e.type == button_release_type)
			etype = ButtonRelease;

		switch (etype) {
		case KeyRelease:
			if (legacy) {
				keycode = e.xkey.keycode;
				state = e.xkey.state;
			} else {
				/* If xinput extension is used, the event struct
				 * needs to be casted properly to get the correct
				 * keycode/state
				 */
				XDeviceKeyEvent *de = (XDeviceKeyEvent *)&e;
				keycode = de->keycode;
				state = de->state;
			}

			if (ignored && (state & ignored)) {
				if (debug)
					printf("ignoring keystroke %d\n",
					    keycode);
				break;
			}

			hide_cursor();
			break;

		case ButtonRelease:
		case MotionNotify:
			show_cursor();
			break;

		case CreateNotify:
			if (legacy) {
				if (debug)
					printf("created new window, "
					    "snooping on it\n");

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
				show_cursor();
				break;

			case XI_RawButtonRelease:
				break;

			default:
				if (debug)
					printf("unknown XI event type %d\n",
					    xie->evtype);
			}

			XFreeEventData(dpy, cookie);
			break;

		default:
			if (debug)
				printf("unknown event type %d\n", e.type);
		}
	}
}

void
hide_cursor()
{
	if (debug)
		printf("keystroke, %shiding cursor\n",
		    (hiding ? "already " : ""));

	if (!hiding) {
		XFixesHideCursor(dpy, DefaultRootWindow(dpy));
		hiding = 1;
	}
}

void
show_cursor()
{
	if (debug)
		printf("mouse moved, %sunhiding cursor\n",
		    (hiding ? "" : "already "));

	if (hiding) {
		XFixesShowCursor(dpy, DefaultRootWindow(dpy));
		hiding = 0;
	}
}

int
snoop_xinput(Window win)
{
	int opcode, event, error, numdevs, i, j;
	int major, minor, rc, rawmotion = 0;
	int ev = 0;
	unsigned char mask[(XI_LASTEVENT + 7)/8];
	XDeviceInfo *devinfo;
	XInputClassInfo *ici;
	XDevice *device;
	XIEventMask evmasks[1];

	if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error)) {
		if (debug)
			warn("XInput extension not available");

		return (0);
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

		if (debug)
			printf("using xinput2 raw motion events\n");
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
				if (debug)
					printf("attaching to keyboard device "
					    "%s (use %d)\n", devinfo[i].name,
					    devinfo[i].use);

				DeviceKeyRelease(device, key_release_type,
				    event_list[ev]); ev++;
				break;

			case ButtonClass:
				if (rawmotion)
					continue;

				if (debug)
					printf("attaching to buttoned device "
					    "%s (use %d)\n", devinfo[i].name,
					    devinfo[i].use);

				DeviceButtonPress(device, button_press_type,
				    event_list[ev]); ev++;
				DeviceButtonRelease(device,
				    button_release_type, event_list[ev]); ev++;
				break;

			case ValuatorClass:
				if (rawmotion)
					continue;

				if (debug)
					printf("attaching to pointing device "
					    "%s (use %d)\n", devinfo[i].name,
					    devinfo[i].use);

				DeviceMotionNotify(device, motion_type,
				    event_list[ev]); ev++;
				break;
			}
		}

		if (XSelectExtensionEvent(dpy, win, event_list, ev)) {
			warn("error selecting extension events");
			return (0);
		}
	}

	return (ev);
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
usage(void)
{
	fprintf(stderr, "usage: %s [-d] [-i mod]\n", __progname);
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
	else {
		fprintf(stderr, "%s: got X error %d\n", __progname,
			e->error_code);
		exit(1);
	}
}
