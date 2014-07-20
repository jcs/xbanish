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
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput.h>

void snoop(Display *, Window);
void snoop_xinput(Display *, Window);
void usage(void);
int swallow_error(Display *, XErrorEvent *);

static int button_press_type = -1;
static int button_release_type = -1;
static int key_press_type = -1;
static int key_release_type = -1;
static int motion_type = -1;

extern char *__progname;

static int debug = 0;
static unsigned char ignored;

int
main(int argc, char *argv[])
{
	Display *dpy;
	int hiding = 0, xinput = 0, ch, i;
	XEvent e;
	struct mod_lookup {
		char *name;
		int mask;
	} mods[] = {
		{"shift", ShiftMask}, {"lock", LockMask},
		{"control", ControlMask}, {"mod1", Mod1Mask},
		{"mod2", Mod2Mask}, {"mod3", Mod3Mask},
		{"mod4", Mod4Mask}, {"mod5", Mod5Mask}
	};

	while ((ch = getopt(argc, argv, "di:p")) != -1)
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
		case 'p':
			xinput = 1;
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

	XSetErrorHandler(swallow_error);

	if (xinput)
		snoop_xinput(dpy, DefaultRootWindow(dpy));
	else
		/* recurse from root window down */
		snoop(dpy, DefaultRootWindow(dpy));

	for (;;) {
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

		switch (etype) {
		case KeyRelease:
			if (ignored && (e.xkey.state & ignored)) {
				if (debug)
					printf("ignored keystroke %d\n",
					    e.xkey.keycode);
				break;
			}

			if (debug)
				printf("keystroke %d, %shiding cursor\n",
				    e.xkey.keycode, (hiding ? "already " :
				    ""));

			if (!hiding) {
				XFixesHideCursor(dpy, DefaultRootWindow(dpy));
				hiding = 1;
			}

			break;

		case ButtonRelease:
		case MotionNotify:
			if (debug)
				printf("mouse moved to %d,%d, %sunhiding "
				    "cursor\n", e.xmotion.x_root,
				    e.xmotion.y_root,
				    (hiding ? "" : "already "));

			if (hiding) {
				XFixesShowCursor(dpy, DefaultRootWindow(dpy));
				hiding = 0;
			}

			break;

		case CreateNotify:
			if (!xinput) {
				if (debug)
					printf("created new window, snooping "
					    "on it\n");

				/* not sure why snooping directly on the window
				 * doesn't work, so snoop on all windows from
				 * its parent (probably root) */
				snoop(dpy, e.xcreatewindow.parent);
			}

			break;

		default:
			if (debug)
				printf("unknown event type %d\n", e.type);
		}
	}
}

void
snoop(Display *dpy, Window win)
{
	Window parent, root, *kids = NULL;
	XTextProperty text_prop;
	XSetWindowAttributes sattrs;
	unsigned int nkids = 0, i;

	/* firefox stops responding to keys when KeyPressMask is used, so
	 * settle for KeyReleaseMask */
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
		snoop(dpy, kids[i]);
	}

done:
	if (kids != NULL)
		XFree(kids); /* hide yo kids */
}

void
snoop_xinput(Display *dpy, Window win)
{
	int opcode, event, error, numdevs, i, j;
	int ev = 0;
	int kp_type = -1, bp_type = -1;
	XDeviceInfo *devinfo;
	XInputClassInfo *ici;
	XDevice *device;

	if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error))
		errx(1, "XInput extension not available");

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

				DeviceKeyPress(device, key_press_type,
				    event_list[ev]); ev++;
				DeviceKeyRelease(device, key_release_type,
				    event_list[ev]); ev++;
				break;

			case ButtonClass:
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
			return;
		}
	}
}

void
usage(void)
{
	fprintf(stderr, "usage: %s [-dp] [-i mod]\n", __progname);
	exit(1);
}

int
swallow_error(Display *dpy, XErrorEvent *e)
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
