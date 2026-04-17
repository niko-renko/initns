# `cgroup/` — cgroup v2 management

Files: `cgroup/cgroup.h`, `cgroup/cgroup.c`.

Constants:
- `CGROUP_ROOT = "/sys/fs/cgroup"`
- `CGROUP_NAME = "initns"` — the parent cgroup under which all per-instance cgroups live.

## Functions

### `init_cgroup(void)`
Called once from `main()`. Mounts cgroup v2 at `/sys/fs/cgroup` (`mount("cgroup", CGROUP_ROOT, "cgroup2", 0, NULL)`); `EBUSY` is tolerated (already mounted). Then `mkdir(/sys/fs/cgroup/initns, 0755)`, tolerating `EEXIST`.

### `int new_cgroup(const char *name)`
`mkdir /sys/fs/cgroup/initns/<name>` and `open` it with `O_DIRECTORY | O_CLOEXEC`. Returns the fd — the caller passes it to `clone3` via `clone_args.cgroup` so the child is spawned directly inside the new cgroup (no post-hoc migration), and then `close`s it.

### `rm_cgroup(const char *name)` / `wait_cgroup_empty(path)` / `rm_cgroup_children(path)`
Three-step teardown:

1. `wait_cgroup_empty(path)` — opens `<path>/cgroup.events`, reads the `populated` line, and if non-zero blocks on `poll(POLLPRI)` on the same fd. The kernel signals `POLLPRI` whenever the `populated`/`frozen` state transitions; we re-read and loop until `populated 0`. Because `populated` on a parent reflects the whole subtree, one wait at the root covers every sub-cgroup the container's own init created.
2. `rm_cgroup_children(path)` — depth-first `rmdir` of every sub-cgroup directory. No retry loop is needed: by the time we get here the subtree is empty, so `rmdir` succeeds on first try.
3. `rmdir(path)` — finally removes the instance's own cgroup.

Any failure along this path calls `die()`. With the populated=0 wait in front, `EBUSY` should never occur; if it does, something is wrong enough to warrant the crash.

### `set_frozen_cgroup(const char *name, int frozen)`
Writes `"1"` or `"0"` to `/sys/fs/cgroup/initns/<name>/cgroup.freeze`. Used to:
- Freeze the running container when the user presses `Ctrl+Alt+J` (`kbd/kbd.c` → `on_ctl()`).
- Unfreeze when the user runs the same instance again from the host shell (`cmd_run` same-instance branch).

### `kill_cgroup(const char *name)`
Writes `"1"` to `/sys/fs/cgroup/initns/<name>/cgroup.kill`, sending `SIGKILL` to every process in the cgroup. Used by `cmd_run` (preempting a different instance) and `cmd_stop`.

## Ordering guarantees

Within a single command handler, the sequence `kill_cgroup → rm_cgroup` is used. `kill_cgroup` returns as soon as the write completes, but processes may still be reaping; `rm_cgroup`'s initial `wait_cgroup_empty` is what actually waits for them to clear — event-driven via `poll(POLLPRI)` on `cgroup.events`, no sleep/retry loop.

A `sync()` call precedes kill/rm in `cmd_run` and `cmd_stop` so that any in-flight writes from the about-to-die container are flushed first.
