# Socket protocol

The daemon listens on `/run/initns.sock` (`AF_UNIX`, `SOCK_STREAM`). `bin/initns` is the reference client.

## Framing

**Request**: the client joins `argv[1..]` with single spaces, appends `\n`, and writes it. Example wire form for `initns new mybox alpine.tar.gz`:

```
new mybox alpine.tar.gz \n
```

(Note the trailing space before `\n` — it is benign; `strtok` on the server side ignores it.)

The server reads up to 255 bytes at a time (`cmd/cmd.c:277`). A single command must fit in one read; there is no multi-read assembly. Multiple commands may be sent over one connection (the `cmd()` loop reads until EOF).

**Response**: a status line followed by end-of-response marker `\n\n`. Valid status lines (`cmd/cmd.c:29-31`):

- `ok\n` — success
- `error\n` — request rejected (bad name, wrong state, missing image, not-running, …)
- `syntax\n` — unknown command or missing argument

For `ls`, the response body is the listing itself (newline-separated), terminated by the `\n\n` marker. The CLI stops reading on the first `\n\n` it sees.

## Commands

| Command              | Args                   | Effect |
|---                   |---                     |---|
| `new <name> <image>` | name, image filename   | Reject if name already exists or image file is missing. Otherwise add to instances file, mkdir rootfs, extract tar, `sync`. |
| `seed <src-dir> <image>` | source directory, image filename | Reject if the target image already exists or `<src-dir>` is missing / not a directory. Otherwise tar the source with `--one-file-system` into `/var/lib/initns/images/<image>`, `sync`. Designed for "snapshot this mounted filesystem as-is" — point it at `/`, a loop-mounted partition, a `pacstrap`ped dir, etc. Submounts on different filesystems (procfs, sysfs, devtmpfs, tmpfs, a separate `/home`, `/boot/efi`) are recorded as empty mountpoint directories. |
| `commit <name> <image>` | name, image filename | Reject if the instance does not exist or the target image file already exists. Otherwise `sync`, tar the rootfs into `/var/lib/initns/images/<image>`, `sync`. Relies on the VT63 freeze invariant (see `@subsystems/cmd.md`) — the caller reached the socket through the host shell, so the container is already frozen. |
| `rm <name>`          | name                   | Reject if unknown. `rm -rf` the rootfs, drop from instances file, `sync`. Does *not* check if running. |
| `run <name>`         | name                   | Reject if unknown. If `state->instance == name`: just unfreeze. If another is running: kill + rm its cgroup first. Then `stop_ctl()`, create cgroup, `clone3` into it, child execs `/sbin/init`. |
| `stop <name>`        | name                   | Reject if unknown or not the currently running instance. Kill cgroup, rm cgroup, clear `state->instance`. |
| `ls image`           | literal `image`        | Print basenames under `/var/lib/initns/images/` separated by `\n`. |
| `ls instance`        | literal `instance`     | Print the raw contents of `/var/lib/initns/instances`. |
| `help`               | (none)                 | Print a built-in usage summary of every command, `\n`-separated. No status line; terminates on the `\n\n` frame like `ls`. |

All commands terminate with `write(out, "\n\n", 2)` plus `fsync(out)` (`cmd/cmd.c:272-273`).

## Concurrency

The socket server handles one connection at a time (`accept` → `cmd` → `close`, then loop). Keyboard-triggered paths (`start_ctl` / `stop_ctl`) run concurrently with command handlers; they coordinate via `state->lock`. See `@architecture.md` for the threading picture.

## Example session

```
$ bin/initns ls image
alpine.tar.gz
debian.tar.gz

$ bin/initns seed /mnt custom.tar       # snapshot a mounted fs into an image
ok

$ bin/initns new sandbox alpine.tar.gz
ok

$ bin/initns run sandbox
ok

# ... press Ctrl+Alt+J to get a host shell on VT63 (container is frozen) ...

$ bin/initns commit sandbox derived.tar   # snapshot the frozen rootfs
ok

$ bin/initns run sandbox   # from host shell, unfreezes + returns console to VT1
ok

$ bin/initns stop sandbox
ok

$ bin/initns rm sandbox
ok
```
