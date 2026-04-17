# `kbd/` + `ctl/` — hotkey and host shell

These two subsystems are paired: the keyboard layer detects `Ctrl+Alt+J` and calls into the control layer, which spawns a bash shell on VT63.

## `kbd/kbd.c` — single-thread hotkey detector

`spawn_kbd()` launches one thread running `kbd()`. That thread owns everything input-related: hotplug, per-device event streams, chord detection. No sub-threads, no per-device state handed across threads.

Startup (in order):

1. `epoll_create1(0)`.
2. `inotify_init1(0)` (blocking — epoll drives it) + `inotify_add_watch("/dev/input", IN_CREATE)`. Added to epoll *before* the initial scan, so a device created mid-scan isn't lost.
3. `scan_existing_devices()` — iterate current `/dev/input/event*` and call `add_device()` on each.

Main loop: `epoll_wait(epfd, evs, 16, -1)` blocks until inotify or some evdev fd is readable. Zero CPU at idle. A sentinel pointer (`&inotify_tag`) in `epoll_data.ptr` distinguishes the inotify fd from keyboard fds; keyboard fds store their `kbd_dev *`.

### Device classification

`classify_kbd(fd)`: takes an already-open fd and uses `ioctl(EVIOCGBIT(0, …))` + `ioctl(EVIOCGBIT(EV_KEY, …))` to check the device exposes `EV_KEY` + `EV_SYN` and supports more than 15 distinct key codes in `1..255`. That threshold rejects mice, touchpads, power buttons, and most non-keyboard `event*` devices.

`add_device(path)`:

- Deduplicates via `find_device(path)` — scan vs inotify can both fire for the same node.
- `open(path, O_RDONLY)` (blocking), then `classify_kbd`; on mismatch, close and return.
- `query_mods(fd)` via `ioctl(EVIOCGKEY, …)` seeds the device's modifier bitmap from the kernel's *current* held-key state, so a keyboard attached (or the daemon started) with Ctrl already held doesn't start at zero.
- Register with epoll and prepend to the `devices` linked list.

`remove_device(dev)` is called on read error or EOF: `epoll_ctl(DEL)`, `close(fd)`, unlink, `free`. Inotify's `IN_CREATE` handles replug.

### Chord detection

Each `kbd_dev` carries a 4-bit mask `mods` of modifiers currently held *on that device*:

```
MOD_LCTRL 0x1   MOD_RCTRL 0x2   MOD_LALT 0x4   MOD_RALT 0x8
MOD_CTRL = LCTRL|RCTRL          MOD_ALT  = LALT|RALT
```

`handle_evdev(dev)`:

- Read one `struct input_event` (blocking; epoll guarantees data is ready).
- `EV_SYN` + `SYN_DROPPED` (kernel buffer overflow) → re-query via `EVIOCGKEY` to rebuild `dev->mods`.
- `EV_KEY` on a modifier: set or clear the corresponding bit based on `ev.value` (0 = release, non-zero = press/autorepeat). Left/right are tracked independently, so releasing RightCtrl while LeftCtrl is still held leaves `MOD_CTRL` set.
- `EV_KEY` on `KEY_J` with `value == 1`: compute `global_mods()` as the OR of every `dev->mods` in the list. If `(global & MOD_CTRL) && (global & MOD_ALT)`, fire `on_ctl()`.
- Cross-keyboard chords (Ctrl on keyboard A, Alt+J on keyboard B) complete because `global_mods` is device-aggregate. A 200 ms `CLOCK_MONOTONIC` debounce (`last_fire_ns`) collapses duplicate fires when two keyboards send the chord simultaneously.

`on_ctl()`:

1. `start_ctl()` — hands off to the control subsystem (see below).
2. Under `state->lock`, if there is a running instance, `set_frozen_cgroup(state->instance, 1)` — pause it so the host shell gets exclusive access to the console.

There is no hotkey for the reverse direction; the host exits VT63 by running `initns run <instance>` or `initns stop <instance>`.

### Why a single thread

The previous design spawned one pthread per keyboard with a nonblocking `read` loop per thread — that pegged one CPU per keyboard at idle and could never see a chord split across two keyboards. The single-thread epoll design makes CPU at idle ~0%, gives one shared modifier state, and removes a whole class of per-thread lifetime bugs.

## `ctl/ctl.c` — spawning the shell

`start_ctl()` — called from the keyboard thread:

1. Under `state->lock`: if `state->ctl` is non-zero (a previous shell is still around), `clone_pkill(state->ctl)` — fork+`execl("/bin/pkill", "-9", "-s", <sid>)` to kill everything in that session — then `waitpid(state->ctl, …)` to reap the dead shell. Spawn a new shell via `clone_shell()` and store its pid in `state->ctl`.
2. `vt_mode(VT_PROCESS)` on `/dev/tty63` — take over VT switching so the kernel won't auto-leave.
3. `vt_switch(63)` — make VT63 active.

`clone_shell()` (fork child path, `ctl.c:26`):

1. `setsid()` — new session so `pkill -s` can address the whole tree later.
2. `open("/dev/tty63", O_RDWR | O_NOCTTY)`, `dup2` it to stdout/stderr.
3. `ioctl(TIOCSCTTY, 1)` — adopt as controlling TTY.
4. `ioctl(KDSETMODE, KD_TEXT)` — make sure the VT is in text (not graphics) mode.
5. `ioctl(KDSKBMODE, K_UNICODE)` — keyboard in Unicode mode.
6. `setenv("PATH", "/bin:/usr/bin")`, `setenv("HOME", "/root")`.
7. `execl("/bin/bash", "bash", NULL)`; if it returns, `die("execl bash")`.

`clone_pkill()` forks, `waitpid`s the pkill helper, and in the child `execl`s `/bin/pkill`; exec failure hits `die("execl pkill")`. Because no generic reaper exists, any `waitpid` failure `die()`s — an `ECHILD` here would be a real bug.

`stop_ctl()`:

1. Under `state->lock`: if `state->ctl` is set, `clone_pkill(state->ctl)` then `waitpid(state->ctl, …)` to reap the shell; clear `state->ctl`.
2. `vt_mode(VT_AUTO)` — return VT control to the kernel.
3. `vt_switch(1)` — back to the primary console.

Any processes the VT63 bash spawned that outlive it get SIGKILLed by the session-wide `pkill` in step 1, but their zombies reparent to PID 1 and nothing reaps them (see `@../architecture.md` → "Child reaping"). That leak is the deliberate tradeoff for not running a generic `SIGCHLD` reaper.

## `ctl/vt.c` — VT ioctls

`vt_switch(int vt)`: open `/dev/tty0`, `ioctl(VT_ACTIVATE, vt)`, `ioctl(VT_WAITACTIVE, vt)`. `EINTR` is retried. `/dev/tty0` is the "current VT" handle; activating a number does the switch.

`vt_mode(int modeval)`: open `/dev/tty63` with `O_NOCTTY`, populate a `struct vt_mode` with `mode=modeval`, `relsig=SIGWINCH`, `acqsig=SIGWINCH`, then `ioctl(VT_SETMODE)`. `VT_PROCESS` means "this process mediates switches"; `VT_AUTO` means "kernel handles it."

`SIGWINCH` is used as the rel/acq signal by this code — not ignored but also not explicitly handled. In practice the shell process receives the signal and ignores it; the VT still switches because `start_ctl`/`stop_ctl` drive the `VT_ACTIVATE` themselves while in `VT_PROCESS` mode.
