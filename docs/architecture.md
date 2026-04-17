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

stdout/stderr are whatever the kernel handed PID 1 (usually `/dev/console`). Diagnostics from `die()` go to `/dev/kmsg` (not stderr) so they land in the kernel ring buffer and can be recovered via pstore across a panic. See `@build-and-run.md` for the logging setup.

Two long-running thread classes exist:

- **Socket server** (`cmd/sock_cmd.c`) — accepts connections on `/run/initns.sock`, runs `cmd()` per connection.
- **Keyboard watcher** (`kbd/kbd.c`) — single thread, `epoll` over an `inotify` watch on `/dev/input/` plus every keyboard `event*` fd. Handles hotplug, tracks modifier state per device, and fires `Ctrl+Alt+J` when any keyboard (or combination of keyboards) completes the chord.

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
keyboard event (Ctrl+Alt+J press, possibly split across devices)
        │
        ▼
kbd/kbd.c: on_ctl()
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

Almost every failure inside the daemon calls `die()`, which writes `<3>initns: <msg>: <strerror>\n` to `/dev/kmsg` and then `exit(1)`s. For PID 1, `exit(1)` is a kernel panic — that's intentional: if cgroup mounting, the socket, or a command handler hits an error path, there is nothing sensible to limp along as. The kmsg write lands in the kernel ring buffer before the panic, so with pstore configured (`efi-pstore` or `ramoops`) the cause survives the reboot as `/sys/fs/pstore/dmesg-*`.

The rmdir path in `cgroup/cgroup.c` waits for `cgroup.events` to report `populated 0` via `poll(POLLPRI)` before walking the tree bottom-up, so `rmdir` never races a still-exiting process.

## Child reaping

Every child PID 1 forks is reaped at the site that forked it — there is no generic `SIGCHLD` handler. The rule: whoever knows the pid does the `waitpid`.

- `clone_tar`, `clone_rm` (in `cmd/cmd.c`) and `clone_pkill` (in `ctl/ctl.c`) fork + `waitpid(pid, NULL, 0)` inline. Short-lived, one call site.
- **VT63 bash** — `state->ctl` holds its pid. Both `start_ctl` (preempt path) and `stop_ctl` `waitpid(state->ctl, NULL, 0)` after `clone_pkill` lands the SIGKILL.
- **Container `/sbin/init`** — `clone_init` returns the pid; `cmd_run` stores it in `state->container`. After `kill_cgroup`, both `cmd_run` (preempt path) and `cmd_stop` `waitpid(state->container, NULL, 0)` before `rm_cgroup`.

Because every wait targets a specific pid and there are no concurrent reapers, `waitpid` never returns `ECHILD` under normal flow — any `ECHILD` is a real bug and `die()`s.

**Known leak:** descendants of the VT63 bash that outlive bash. `clone_pkill` SIGKILLs the whole session (`pkill -9 -s <sid>`), so they do die, but their zombies reparent to PID 1 and nobody collects them. A user who backgrounds `sleep 1000` in their host shell and then lets the shell exit will leave one zombie per backgrounded process. The tradeoff is explicit: accept a small, bounded leak in exchange for not installing a generic reaper that would race every explicit wait. Processes inside the container don't leak — they live in their own PID namespace, and when its PID 1 (`/sbin/init`) dies the kernel tears the namespace down.

Every `fork`/`clone3` child also has a `die("execl …")` immediately after its `execl`, so a missing binary (`/bin/tar`, `/bin/rm`, `/bin/pkill`, `/bin/bash`, `/sbin/init`) terminates the child with a kmsg line rather than silently falling back into the parent's code path.

## Fd hygiene

Every fd the daemon opens is created `O_CLOEXEC` (`SOCK_CLOEXEC` for the listening socket and `accept4`; `EPOLL_CLOEXEC` / `IN_CLOEXEC` in `kbd/`; `"e"` mode for `fopen` in `set/`). The kernel then closes all of them automatically at each `execl`, so there is no generic fd-cleanup loop in any fork child. The one exception is `clone_shell`: it wants stdin/stdout/stderr pointing at `/dev/tty63` after exec, so it opens tty63 with `O_CLOEXEC` and `dup2`s it onto fds 0/1/2 — the dup'd copies have `CLOEXEC` cleared by `dup2` and survive into bash, while the original fd closes on exec.
