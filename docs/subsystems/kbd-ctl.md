# `kbd/` + `ctl/` — hotkey and host shell

These two subsystems are paired: the keyboard layer detects `Ctrl+Alt+J` and calls into the control layer, which spawns a bash shell on VT63.

## `kbd/kbd.c` — device discovery

`spawn_kbd()` launches a detached thread running `kbd()`:

1. `inotify_init1(IN_NONBLOCK)` + `inotify_add_watch("/dev/input", IN_CREATE)`.
2. `scan_existing_devices()` — iterate current `/dev/input/event*` so we pick up keyboards that existed before the daemon started.
3. Loop: read inotify events; for each new `event*` node, call `on_device_added(path)`.

`is_kbd(path)`: opens the device and uses `ioctl(fd, EVIOCGBIT(0, ...))` and `EVIOCGBIT(EV_KEY, ...)` to check the device exposes `EV_KEY` + `EV_SYN` and counts how many key codes in `1..255` are supported. If more than 15, treat it as a keyboard. This rejects mice, touchpads, and most non-keyboard `event*` devices.

For each accepted device, `spawn_seq_listener(path)` starts a thread (see below).

Non-blocking reads on the inotify fd get `EAGAIN` — the loop sleeps `usleep(200000)` (200 ms) and retries.

## `kbd/seq_listener.c` — hotkey detection

One thread per keyboard. `spawn_seq_listener` allocates a `seq_listener_args` struct on the heap, hands ownership to the worker thread via `pthread_create`, and `pthread_detach`s the thread (nobody joins). Inside the worker, the device path is copied to a stack buffer and the heap struct is `free`d before the event loop starts, so no dangling pointer can outlive the thread.

The worker opens the `event*` file `O_RDONLY | O_NONBLOCK` and reads a stream of `struct input_event`:

- Tracks the press/release state of `KEY_LEFTCTRL`/`KEY_RIGHTCTRL` and `KEY_LEFTALT`/`KEY_RIGHTALT`.
- When `KEY_J` arrives with `value == 1` (press) *and* both Ctrl and Alt are currently held, calls `on_ctl()`.

`on_ctl()`:

1. `start_ctl()` — hands off to the control subsystem (see below).
2. Under `state->lock`, if there is a running instance, `set_frozen_cgroup(state->instance, 1)` — pause it so the host shell gets exclusive access to the console.

There is no hotkey for the reverse direction; the host exits VT63 by running `initns run <instance>` or `initns stop <instance>`.

## `ctl/ctl.c` — spawning the shell

`start_ctl()` — called from the keyboard thread:

1. Under `state->lock`: if `state->ctl` is non-zero (a previous shell is still around), `clone_pkill(state->ctl)` — fork+`execl("/bin/pkill", "-9", "-s", <sid>)` to kill everything in that session. Then spawn a new shell via `clone_shell()` and store its pid in `state->ctl`.
2. `vt_mode(VT_PROCESS)` on `/dev/tty63` — take over VT switching so the kernel won't auto-leave.
3. `vt_switch(63)` — make VT63 active.

`clone_shell()` (fork child path, `ctl.c:26`):

1. `setsid()` — new session so `pkill -s` can address the whole tree later.
2. `open("/dev/tty63", O_RDWR | O_NOCTTY)`, `dup2` it to stdout/stderr.
3. `ioctl(TIOCSCTTY, 1)` — adopt as controlling TTY.
4. `ioctl(KDSETMODE, KD_TEXT)` — make sure the VT is in text (not graphics) mode.
5. `ioctl(KDSKBMODE, K_UNICODE)` — keyboard in Unicode mode.
6. `setenv("PATH", "/bin:/usr/bin")`, `setenv("HOME", "/root")`.
7. `execl("/bin/bash", "bash", NULL)`.

`stop_ctl()`:

1. Under `state->lock`: if `state->ctl` is set, `clone_pkill(state->ctl)`; clear it.
2. `vt_mode(VT_AUTO)` — return VT control to the kernel.
3. `vt_switch(1)` — back to the primary console.

## `ctl/vt.c` — VT ioctls

`vt_switch(int vt)`: open `/dev/tty0`, `ioctl(VT_ACTIVATE, vt)`, `ioctl(VT_WAITACTIVE, vt)`. `EINTR` is retried. `/dev/tty0` is the "current VT" handle; activating a number does the switch.

`vt_mode(int modeval)`: open `/dev/tty63` with `O_NOCTTY`, populate a `struct vt_mode` with `mode=modeval`, `relsig=SIGWINCH`, `acqsig=SIGWINCH`, then `ioctl(VT_SETMODE)`. `VT_PROCESS` means "this process mediates switches"; `VT_AUTO` means "kernel handles it."

`SIGWINCH` is used as the rel/acq signal by this code — not ignored but also not explicitly handled. In practice the shell process receives the signal and ignores it; the VT still switches because `start_ctl`/`stop_ctl` drive the `VT_ACTIVATE` themselves while in `VT_PROCESS` mode.
