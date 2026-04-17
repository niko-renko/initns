# Architecture

`initns` is a container-like init system for Linux. It runs as PID 1, exposes a Unix-socket command interface to create and launch container instances, and provides a hotkey-triggered "control shell" on VT63 for host access while a container is running.

It is deliberately small: two binaries, no external runtime dependencies beyond `/bin/tar`, `/bin/rm`, `/bin/pkill`, and `/bin/bash`.

## Binaries

- **`bin/init`** — the daemon. Expected to run as PID 1 on the host. Built from `main.c`.
- **`bin/initns`** — a thin CLI client that speaks the socket protocol. Built from `main_initns.c`.

Both link the same set of common modules (`set/`, `cmd/`, `kbd/`, `state/`, `cgroup/`, `ctl/`, `common.c`); `initns` itself only uses `common.c` at runtime but links the rest for simplicity.

## Process / thread model

`main.c` does the following on start:

1. Creates `State` and installs it into thread-local storage (`state/`).
2. `mkdir`s `/var/lib/initns`, `/var/lib/initns/images`, `/var/lib/initns/rootfs`.
3. `init_cgroup()` — mount cgroup v2 and create the `initns` parent cgroup.
4. `spawn_kbd()` — start the keyboard watcher thread.
5. `spawn_sock_cmd()` — start the Unix-socket command server thread.
6. `pause()` forever.

stdout/stderr are whatever the kernel handed PID 1 (usually `/dev/console`); `perror` output from `die()` and `sock_cmd`'s `accept` failure lands there, nothing is captured to a file.

Three long-running thread classes exist:

- **Socket server** (`cmd/sock_cmd.c`) — accepts connections on `/run/initns.sock`, runs `cmd()` per connection.
- **Keyboard watcher** (`kbd/kbd.c`) — uses `inotify` on `/dev/input/` to discover keyboards as they appear, and starts a per-device listener thread (`kbd/seq_listener.c`) for each.
- **Per-keyboard sequence listeners** — one thread per keyboard device, watching for the `Ctrl+Alt+J` chord.

All threads are fire-and-forget (never joined). They share the single `State` through pthread TLS; its mutex protects `state->instance` (the currently running container) and `state->ctl` (the PID of the host shell on VT63, if any).

## Data flow: starting a container

```
initns CLI ──▶ /run/initns.sock ──▶ cmd/sock_cmd.c: accept()
                                           │
                                           ▼
                                    cmd/cmd.c: cmd_run()
                                           │
                                           ├─ state->lock
                                           ├─ if same instance running: unfreeze cgroup, return
                                           ├─ if different: kill + rm its cgroup
                                           ├─ state->instance = name
                                           ├─ stop_ctl() (leave VT63 shell, back to VT1)
                                           ├─ new_cgroup(name)  → /sys/fs/cgroup/initns/<name>
                                           └─ clone3(CLONE_INTO_CGROUP|NEWPID|NEWNS|NEWCGROUP)
                                                   │
                                                   └─ child: mount private,
                                                             bind-mount rootfs on itself,
                                                             pivot_root, umount old,
                                                             execl("/sbin/init")
```

## Data flow: the control-shell hotkey

```
keyboard event (Ctrl+Alt+J press)
        │
        ▼
kbd/seq_listener.c: on_ctl()
        │
        ├─ ctl/ctl.c: start_ctl()
        │     ├─ if a prior shell exists, pkill -9 -s <sid>
        │     ├─ fork → setsid → open /dev/tty63 → TIOCSCTTY
        │     ├─ KDSETMODE=KD_TEXT, KDSKBMODE=K_UNICODE
        │     ├─ execl("/bin/bash")
        │     ├─ vt_mode(VT_PROCESS) on /dev/tty63
        │     └─ vt_switch(63) via /dev/tty0
        │
        └─ set_frozen_cgroup(state->instance, 1)   (pause the running container)
```

The container is resumed either by `initns run <name>` (which calls `set_frozen_cgroup(name, 0)` and `stop_ctl()` to return to VT1) or by explicitly `stop`ping it.

## Why VT63

VT63 is outside the range most setups wire to a getty, so it can host an interactive bash session without clashing with the active container's TTYs. The daemon puts VT63 into `VT_PROCESS` mode while the shell lives so the kernel won't auto-switch away from it; when the shell stops, mode goes back to `VT_AUTO` and the daemon switches to VT1.

## Kernel interfaces used

- **Namespaces + cgroup placement:** `clone3` with `CLONE_INTO_CGROUP | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWCGROUP`.
- **Root pivot:** `mount(MS_REC|MS_PRIVATE)`, `mount(MS_BIND|MS_REC)`, `pivot_root`, `umount2(MNT_DETACH)`.
- **Cgroup v2 control files:** `cgroup.freeze`, `cgroup.kill`.
- **VT:** `VT_ACTIVATE`, `VT_WAITACTIVE`, `VT_SETMODE` on `/dev/tty0`.
- **TTY:** `TIOCSCTTY`, `KDSETMODE=KD_TEXT`, `KDSKBMODE=K_UNICODE` on `/dev/tty63`.
- **Input:** `ioctl(EVIOCGBIT)` to classify devices as keyboards (must expose `EV_KEY`+`EV_SYN` and more than 15 distinct keys).
- **Device discovery:** `inotify` on `/dev/input`.

## Error policy

Almost every failure inside the daemon calls `die()` (perror + `exit(1)`). This is intentional for PID 1: if cgroup mounting or the socket fails, there is nothing sensible to recover to. The rmdir path in `cgroup/cgroup.c` waits for `cgroup.events` to report `populated 0` via `poll(POLLPRI)` before walking the tree bottom-up, so `rmdir` never races a still-exiting process.
