# `cgroup/` — cgroup v2 management

Files: `cgroup/cgroup.h`, `cgroup/cgroup.c`.

Constants:
- `CGROUP_ROOT = "/sys/fs/cgroup"`
- `CGROUP_NAME = "initns"` — the parent cgroup under which all per-instance cgroups live.

## Functions

### `init_cgroup(void)`
Called once from `main()`. Mounts cgroup v2 at `/sys/fs/cgroup` (`mount("cgroup", CGROUP_ROOT, "cgroup2", 0, NULL)`); `EBUSY` is tolerated (already mounted). Then `mkdir(/sys/fs/cgroup/initns, 0755)`, tolerating `EEXIST`.

### `int new_cgroup(const char *name)`
`mkdir /sys/fs/cgroup/initns/<name>` and `open` it with `O_DIRECTORY`. Returns the fd — the caller passes it to `clone3` via `clone_args.cgroup` so the child is spawned directly inside the new cgroup (no post-hoc migration), and then `close`s it.

### `rm_cgroup(const char *name)` / `rm_cgroup_internal(path)` / `rm_poll(path)`
`rm_cgroup` depth-first traverses the cgroup directory, recursing into subcgroups and calling `rm_poll(path)` on each subdir and finally the root.

`rm_poll` loops `rmdir(path)`:
- success → return
- `EBUSY` → `usleep(1000)` (1 ms) and retry
- any other errno → `die`

`EBUSY` happens when processes in the cgroup are still terminating; the loop waits for them. There is no upper bound on retries — if something in the cgroup refuses to die the daemon hangs here.

### `set_frozen_cgroup(const char *name, int frozen)`
Writes `"1"` or `"0"` to `/sys/fs/cgroup/initns/<name>/cgroup.freeze`. Used to:
- Freeze the running container when the user presses `Ctrl+Alt+J` (`kbd/seq_listener.c` → `on_ctl()`).
- Unfreeze when the user runs the same instance again from the host shell (`cmd_run` same-instance branch).

### `kill_cgroup(const char *name)`
Writes `"1"` to `/sys/fs/cgroup/initns/<name>/cgroup.kill`, sending `SIGKILL` to every process in the cgroup. Used by `cmd_run` (preempting a different instance) and `cmd_stop`.

## Ordering guarantees

Within a single command handler, the sequence `kill_cgroup → rm_cgroup` is used. `kill_cgroup` returns as soon as the write completes, but processes may still be reaping; `rm_cgroup`'s internal `rm_poll` is what actually waits for them to clear.

A `sync()` call precedes kill/rm in `cmd_run` and `cmd_stop` so that any in-flight writes from the about-to-die container are flushed first.
