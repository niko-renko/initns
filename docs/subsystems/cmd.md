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
3. Matches one of `new` / `rm` / `run` / `stop` / `ls` and calls the handler.
4. If no match or missing required arg, writes `syntax\n`.
5. Always appends `\n\n` and `fsync`es.

Note: the current dispatch writes `syntax` after handling too if `arg` is missing for commands other than `new`, because it falls through the `goto syntax` path only when `valid == 0`. Handlers write their own `ok`/`error` before the trailing `\n\n`.

## Handlers

### `cmd_new(out, name, image)` — `cmd.c:94`
- Rejects if `name` already in `/var/lib/initns/instances` **or** the image file does not exist at `/var/lib/initns/images/<image>`.
- `file_add` the name, `mkdir` the rootfs dir, `clone_tar` (fork+`execl("/bin/tar", ..., "--strip-components=1", "-C", rootfs)`), `sync`.

### `cmd_rm(out, name)` — `cmd.c:114`
- Rejects if not present in the instances file.
- `clone_rm` (fork+`execl("/bin/rm", "-rf", rootfs)`), then `file_remove`, then `sync`.
- Does **not** check whether the instance is currently running. Callers are expected to `stop` first.

Both `clone_tar` and `clone_rm` block on `waitpid(pid, NULL, 0)` for the forked helper — any failure (including `ECHILD`) `die()`s, since no other reaper exists to race with them. The child exec paths end in `die("execl tar")` / `die("execl rm")` so a missing binary produces a kmsg line instead of silently continuing in the caller's control flow.

### `cmd_run(out, name)` — `cmd.c:131`
Core of the system. Under `state->lock`:

- If `name == state->instance`: unfreeze its cgroup, release lock, reply `ok`, `stop_ctl()`. Return. (This is the "resume from VT63" path.)
- Otherwise, if `state->instance` is non-empty: `sync`, `kill_cgroup`, `waitpid(state->container, …)` to reap the previous container's `/sbin/init`, then `rm_cgroup`.
- Set `state->instance = name`, release lock.
- Reply `ok`, then `stop_ctl()` (returns console to VT1 if a shell was up).
- `new_cgroup(name)` → returns an `O_DIRECTORY` fd into `/sys/fs/cgroup/initns/<name>`.
- `clone_init(cgroup_fd, name)` — see below; its return value is the new container's pid.
- Re-acquire the lock, store the pid in `state->container`, release.

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

## External processes invoked

- `/bin/tar xf <img> --strip-components=1 -C <rootfs>` (image extraction)
- `/bin/rm -rf <rootfs>` (instance deletion)
- `/sbin/init` inside the container (the pivoted rootfs must provide this)
