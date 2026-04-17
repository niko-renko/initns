# initns

A minimal container init system for Linux. `bin/init` runs as PID 1 and manages one container at a time through cgroup v2 + `clone3` namespaces, pivoting into `/sbin/init` inside an extracted tarball rootfs. `bin/initns` is a CLI client speaking a line-oriented protocol over `/run/initns.sock`. `Ctrl+Alt+J` on any keyboard freezes the running container and drops a bash shell on VT63 for host access.

## Docs index

Start with architecture, then protocol / on-disk layout for the big picture; dive into subsystem docs when touching that code.

- @../docs/architecture.md — binaries, threading model, data flow, kernel interfaces.
- @../docs/build-and-run.md — `make` targets, install paths, lifecycle of an instance.
- @../docs/protocol.md — `/run/initns.sock` wire format, commands, responses.
- @../docs/on-disk-layout.md — `/var/lib/initns/`, `/sys/fs/cgroup/initns/`, device files touched.
- @../docs/subsystems/cmd.md — `cmd/` socket server, command handlers, `clone3`+`pivot_root` path.
- @../docs/subsystems/cgroup.md — `cgroup/` v2 mount, create/freeze/kill/remove.
- @../docs/subsystems/kbd-ctl.md — `kbd/` hotkey detection and `ctl/` VT63 host shell.
- @../docs/subsystems/state-set.md — shared `State` + flat-file set utilities.

## Source layout

```
main.c             entry for bin/init (daemon, PID 1)
main_initns.c      entry for bin/initns (CLI client)
common.{c,h}       ROOT + SOCK_PATH constants, die(), clean_fds()
Makefile           two targets: init, initns

cmd/               socket server + command handlers + container bring-up
cgroup/            cgroup v2 mount/create/freeze/kill/remove
kbd/               inotify on /dev/input + Ctrl+Alt+J listener per keyboard
ctl/               bash shell on VT63, VT switching
state/             pthread-TLS shared State (lock, ctl pid, current instance)
set/               line-oriented add/remove/contains for /var/lib/initns/instances
```
