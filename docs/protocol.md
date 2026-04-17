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
| `rm <name>`          | name                   | Reject if unknown. `rm -rf` the rootfs, drop from instances file, `sync`. Does *not* check if running. |
| `run <name>`         | name                   | Reject if unknown. If `state->instance == name`: just unfreeze. If another is running: kill + rm its cgroup first. Then `stop_ctl()`, create cgroup, `clone3` into it, child execs `/sbin/init`. |
| `stop <name>`        | name                   | Reject if unknown or not the currently running instance. Kill cgroup, rm cgroup, clear `state->instance`. |
| `ls image`           | literal `image`        | Print basenames under `/var/lib/initns/images/` separated by `\n`. |
| `ls instance`        | literal `instance`     | Print the raw contents of `/var/lib/initns/instances`. |

All commands terminate with `write(out, "\n\n", 2)` plus `fsync(out)` (`cmd/cmd.c:272-273`).

## Concurrency

The socket server handles one connection at a time (`accept` → `cmd` → `close`, then loop). Keyboard-triggered paths (`start_ctl` / `stop_ctl`) run concurrently with command handlers; they coordinate via `state->lock`. See `@architecture.md` for the threading picture.

## Example session

```
$ bin/initns ls image
alpine.tar.gz
debian.tar.gz

$ bin/initns new sandbox alpine.tar.gz
ok

$ bin/initns run sandbox
ok

# ... press Ctrl+Alt+J to get a host shell on VT63 (container is frozen) ...

$ bin/initns run sandbox   # from host shell, unfreezes + returns console to VT1
ok

$ bin/initns stop sandbox
ok

$ bin/initns rm sandbox
ok
```
