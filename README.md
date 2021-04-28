## xbanish

xbanish hides the mouse cursor when you start typing, and shows it again when
the mouse cursor moves or a mouse button is pressed.
This is similar to xterm's `pointerMode` setting, but xbanish works globally in
the X11 session.

unclutter's -keystroke mode is supposed to do this, but it's
[broken](https://bugs.launchpad.net/ubuntu/+source/unclutter/+bug/54148).
I looked into fixing it, but unclutter was abandoned so I wrote xbanish.

The name comes from
[ratpoison's](https://www.nongnu.org/ratpoison/)
"banish" command that sends the cursor to the corner of the screen.

### Implementation

If the XInput extension is supported, xbanish uses it to request input from all
attached keyboards and mice.
If XInput 2.2 is supported, raw mouse movement and button press inputs are
requested which helps detect cursor movement while in certain applications such
as Chromium.

If Xinput is not available, xbanish recurses through the list of windows
starting at the root, and calls `XSelectInput()` on each window to receive
notification of mouse motion, button presses, and key presses.

In response to any available keyboard input events, the cursor is hidden.
On mouse movement or button events, the cursor is shown.

xbanish initially hid the cursor by calling `XGrabPointer()` with a blank
cursor image, similar to unclutter's -grab mode, but this had problematic
interactions with certain X applications.
For example, xlock could not grab the pointer and sometimes didn't lock,
xwininfo wouldn't work at all, Firefox would quickly hide the Awesome Bar
dropdown as soon as a key was pressed, and xterm required two middle-clicks to
paste the clipboard contents.

To avoid these problems and simplify the implementation, xbanish now uses the
modern
[`Xfixes` extension](http://cgit.freedesktop.org/xorg/proto/fixesproto/plain/fixesproto.txt)
to easily hide and show the cursor with `XFixesHideCursor()` and
`XFixesShowCursor()`.
