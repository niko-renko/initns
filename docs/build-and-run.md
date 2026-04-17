# Build and run

## Build

```
make init     # produces bin/init     (the daemon / PID 1)
make initns   # produces bin/initns   (the CLI client)
make clean    # rm -f bin/*
```

Flags: `-Wall -Wextra -Wunused -O2`. No headers outside glibc and `linux/*` UAPI.

The `bin/` directory is expected to exist and is gitignored. Create it once with `mkdir bin` before the first build.

## Install (expected layout)

`initns` hardcodes two paths via `common.h`:

- `ROOT = /var/lib/initns`
- `SOCK_PATH = /run/initns.sock`

`bin/init` is designed to run as PID 1. Typical deployment: install it at `/sbin/init` on the *host* rootfs (or boot with `init=/path/to/bin/init`). `bin/initns` goes anywhere on the host `$PATH`.

Container images are plain tarballs placed under `/var/lib/initns/images/<name>.tar.gz` (or `.tar` — `tar xf` autodetects). Each image is expected to contain `/sbin/init` at its root, since that is what `clone_init()` execs after `pivot_root` (`cmd/cmd.c:91`).

## Runtime prerequisites

- Linux kernel with cgroup v2 and the `clone3` `CLONE_INTO_CGROUP` flag.
- `/sys/fs/cgroup` available (it gets mounted as cgroup2 if not already).
- `/dev/input/event*` nodes readable by the daemon (root) for keyboard discovery.
- `/dev/tty0` and `/dev/tty63` present.
- `/bin/tar`, `/bin/rm`, `/bin/pkill`, `/bin/bash` present on the host rootfs (invoked via `fork`+`exec`).

## Lifecycle

1. Place an image at `/var/lib/initns/images/<image>.tar.gz` — either a `./`-prefixed tar you produce yourself (see `@protocol.md` for the format), or `initns seed <src-dir> <image>` on the current host to snapshot a mounted filesystem as-is via `tar --one-file-system`.
2. `initns new <instance> <image>` — creates `/var/lib/initns/rootfs/<instance>/` by extracting the tar with `--strip-components=1` and records the name in `/var/lib/initns/instances`.
3. `initns run <instance>` — starts it via `clone3` into a fresh cgroup + namespaces and `execl("/sbin/init")`. If the same instance is already running it is unfrozen; if a *different* instance is running, it is killed and its cgroup removed.
4. Press `Ctrl+Alt+J` at any keyboard: the running container is frozen and a bash shell is spawned on VT63 for host access. The kernel switches the console to VT63.
5. `initns run <same-instance>` (from that shell or another TTY) — unfreezes the container and returns the console to VT1.
6. `initns commit <instance> <image>` — from the VT63 host shell, snapshot the (frozen) rootfs into `/var/lib/initns/images/<image>`. The resulting tarball is fed straight back to `initns new` on any host running `initns`. Rejects if the target file already exists.
7. `initns stop <instance>` — kills the cgroup, removes it, clears `state->instance`.
8. `initns rm <instance>` — removes rootfs and drops the name from the instances file. Must not be the currently running one (the command does not check this; stop first).

## Logging

Fatal errors go through `die()`, which writes `<3>initns: <msg>: <strerror(errno)>\n` to `/dev/kmsg` and then `exit(1)`s. The `<3>` priority is `KERN_ERR`, so the line appears on the console even under `quiet` and lands in the kernel ring buffer in chronological order with kernel messages. To read it live: `dmesg | grep initns`.

Since `exit(1)` from PID 1 is a kernel panic, capturing the kmsg across the reboot requires **pstore**. On EFI systems the easiest backend is `efi-pstore`: add `efi_pstore.pstore_disable=0` to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`, run `grub-mkconfig -o /boot/grub/grub.cfg`, and reboot. After any future panic, `sudo cat /sys/fs/pstore/dmesg-efi-*` shows the tail of the ring buffer — including every `initns:` kmsg line — preserved across the reboot.

No file-based log. No rotation. No syslog.
