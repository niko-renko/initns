# `cmd/` — command server and handlers

Files:
- `cmd/sock_cmd.c` — Unix-socket listener thread.
- `cmd/cmd.c` — command parsing + handlers + container bring-up.
- `cmd/cmd.h` — one prototype: `void cmd(int in, int out)`.

## Socket server (`sock_cmd.c`)

`spawn_sock_cmd()` starts one thread that:

1. Copies the caller's `State` into its own TLS slot.
2. Creates `AF_UNIX`/`SOCK_STREAM` socket, `unlink`s `/run/initns.sock`, `bind`s, `listen(5)`.
3. Loops on `accept` → `cmd(cfd, cfd)` → `close(cfd)`. `EINTR` on accept is retried; any other accept error calls `die()` (kmsg write + `exit`, which is a kernel panic for PID 1 — a broken command channel is not a state worth limping along in).

Only one connection is served at a time; there is no per-connection thread.

## Command dispatch (`cmd.c`)

`cmd(in, out)` reads up to 255-byte chunks and calls `accept_cmd()` on each. `accept_cmd()`:

1. Null-terminates the buffer; trims the first `\n`.
2. `strtok(" ")` extracts `cmd`, `arg`, `arg2`.
3. A flat `if`/`else if` chain dispatches to one of `new` / `commit` / `rm` / `run` / `stop` / `ls` / `help`. `help` is checked first among one-word verbs (it takes no args); then the `!arg` guard rejects everything else that lacks an argument. Missing required args or an unknown verb falls through to the terminal `else` and writes `syntax\n`.
4. Always appends `\n\n` and `fsync`es. Handlers write their own `ok`/`error` before the framing.

## Handlers

### `cmd_new(out, name, image)` — `cmd.c:94`
- Rejects if `name` already in `/var/lib/initns/instances` **or** the image file does not exist at `/var/lib/initns/images/<image>`.
- `file_add` the name, `mkdir` the rootfs dir, `clone_tar_extract` (fork+`execl("/bin/tar", ..., "--strip-components=1", "-C", rootfs)`), `sync`.

### `cmd_commit(out, name, image_name)`
- Rejects if the instance is not in `/var/lib/initns/instances` **or** the target file `/var/lib/initns/images/<image>` already exists (overwriting a committed image silently would be easy to do by accident and hard to recover from).
- `clone_tar_create` — fork+`execl("/bin/tar", "cf", image, "-C", rootfs, ".")` — then `sync` so the new image is durable before reply. No pre-tar sync: the container is already frozen (VT63 invariant), so there are no in-flight writes to chase, and `tar` reads through the VFS anyway.
- Does **not** take `state->lock` or freeze the cgroup. The only way a caller reaches `/run/initns.sock` is through the VT63 bash shell, which is only entered via `Ctrl+Alt+J` → `on_ctl()` → `set_frozen_cgroup(state->instance, 1)`. So by the time `cmd_commit` runs, the container's rootfs is already quiescent. See `@../subsystems/kbd-ctl.md`.

### `cmd_rm(out, name)` — `cmd.c:114`
- Rejects if not present in the instances file.
- `clone_rm` (fork+`execl("/bin/rm", "-rf", rootfs)`), then `file_remove`, then `sync`.
- Does **not** check whether the instance is currently running. Callers are expected to `stop` first.

`clone_tar_extract`, `clone_tar_create`, and `clone_rm` all block on `waitpid(pid, NULL, 0)` for the forked helper — any failure (including `ECHILD`) `die()`s, since no other reaper exists to race with them. The child exec paths end in `die("execl tar")` / `die("execl rm")` so a missing binary produces a kmsg line instead of silently continuing in the caller's control flow.

### `cmd_run(out, name)` — `cmd.c:131`
Core of the system. Under `state->lock`:

- If `name == state->instance`: unfreeze its cgroup, release lock, reply `ok`, `stop_ctl()`. Return. (This is the "resume from VT63" path.)
- Otherwise, if `state->instance` is non-empty: `sync`, `kill_cgroup`, `waitpid(state->container, …)` to reap the previous container's `/sbin/init`, then `rm_cgroup`.
- Still under the lock: `new_cgroup(name)` → returns an `O_DIRECTORY` fd into `/sys/fs/cgroup/initns/<name>`; then `clone_init(cgroup_fd, name)` to fork the container. Store the returned pid in `state->container`, set `state->instance = name`.
- Release the lock. Reply `ok`, then `stop_ctl()` (returns console to VT1 if a shell was up).

The cgroup creation and `clone3` are done under the lock so `state->instance` never names a cgroup that doesn't yet exist — otherwise a `Ctrl+Alt+J` fire in that window would call `set_frozen_cgroup` on a missing path and `die()` PID 1. `clone3` returns to the parent as soon as the child is forked (the child's mount/pivot/exec work happens concurrently), so the lock hold stays short.

### `clone_init(cgroup, name) -> pid_t` — `cmd.c:60`
Uses `syscall(SYS_clone3, &args, sizeof(args))` with:
```
flags       = CLONE_INTO_CGROUP | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWCGROUP
exit_signal = SIGCHLD
cgroup      = <fd from new_cgroup>
```

Child path:
1. `mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL)` — detach from host mount propagation.
2. `mount(rootfs, rootfs, NULL, MS_BIND|MS_REC, NULL)` — `pivot_root` requires the new root to itself be a mount point.
3. `syscall(SYS_pivot_root, rootfs, rootfs/mnt)` — old root ends up at `/mnt`.
4. `umount2("/mnt", MNT_DETACH)` — lazy-unmount the old root so it can be reaped later.
5. `execl("/sbin/init", "init", NULL)`; if exec returns, `die("execl init")`.

A failed `clone3` in the parent calls `die("clone")`. The parent returns the child pid up to `cmd_run`, which stashes it in `state->container` so that `cmd_stop` / the preempt branch of `cmd_run` can reap it with `waitpid(state->container, …)` after `kill_cgroup`.

No user-namespace remap, no mount of `/proc` / `/sys` / `/dev` — the image is expected to contain whatever the contained `/sbin/init` needs, or to mount those itself.

### `cmd_stop(out, name)` — `cmd.c:210`
Under `state->lock`, rejects unless `name == state->instance`. Then `sync`, `kill_cgroup`, `waitpid(state->container, …)`, clear `state->container`, `rm_cgroup`, clear `state->instance`. Reply `ok`.

### `cmd_ls(out, "image" | "instance")` — `cmd.c:167`
- `image`: `opendir("/var/lib/initns/images")`, write each non-dot entry, `\n`-separated (no trailing newline between last entry and the `\n\n` framing).
- `instance`: raw `read`/`write` the contents of `/var/lib/initns/instances`.
- Anything else: `syntax\n`.

### `cmd_help(out)`
- Writes a static, built-in usage summary (one command per line) directly to the client. Like `cmd_ls`, it emits no `ok`/`error` status line — the body *is* the response, terminated by the `\n\n` frame added by `accept_cmd`. Edit the string literal in `cmd_help` when adding or removing commands.

## External processes invoked

- `/bin/tar xf <img> --strip-components=1 -C <rootfs>` (image extraction)
- `/bin/tar cf <img> -C <rootfs> .` (image creation, `commit`)
- `/bin/rm -rf <rootfs>` (instance deletion)
- `/sbin/init` inside the container (the pivoted rootfs must provide this)
