# `state/` and `set/` — shared state and flat-file sets

Two small utility modules used by the rest of the daemon.

## `state/` — thread-local shared state

Files: `state/state.h`, `state/state.c`.

```c
typedef struct state {
    pthread_mutex_t lock;
    pid_t ctl;              // pid of the host shell on VT63, or 0 if none
    pid_t container;        // pid of the currently running /sbin/init, or 0
    char *instance;         // name of the currently running container, "" if none
} State;
```

API:

- `State *init_state(void)` — allocate, `pthread_mutex_init`, set `ctl=0`, `container=0`, `instance=""`. Called once from `main()`.
- `void set_state(State *s)` — install the State into the calling thread's TLS slot (`pthread_key_create` guarded by `pthread_once`).
- `State *get_state(void)` — read the TLS slot.

Every thread spawned by the daemon (socket server, keyboard watcher) calls `set_state(arg)` at its top, so `get_state()` works anywhere. The struct is shared — TLS holds a pointer to the same `State`, not per-thread copies — and all mutation is done under `state->lock`.

### What the lock protects

- `state->instance` (mutated in `cmd_run`, `cmd_stop`; read + written in `on_ctl`)
- `state->container` (mutated in `cmd_run`, `cmd_stop`)
- `state->ctl` (mutated in `start_ctl`, `stop_ctl`)

The mutex is held across the write-to-client `ok`/`error` inside `cmd_run`'s same-instance unfreeze path, and across `new_cgroup` + `clone3` in the new-instance path — so `state->instance` and the cgroup it names are always consistent to concurrent observers (e.g. a `Ctrl+Alt+J` hotkey trying to freeze it). `clone3` returns as soon as the child is forked, so the hold stays short.

## `set/` — append/remove/contains for line-oriented files

Files: `set/set.h`, `set/set.c`.

Used only for `/var/lib/initns/instances`, but generic:

- `int file_add(const char *path, const char *s)` — idempotent append of `s` as its own line. Creates the file if it does not exist.
- `int file_remove(const char *path, const char *s)` — remove the line equal to `s`, if present.
- `int file_contains(const char *path, const char *s)` — non-zero if `s` appears as a line.
- `int file_set(const char *path, const char **strings, size_t count)` — replace the entire file with the given lines.

Implementation sketch: load the file into a `Node { char *str; Node *next; }` linked list (max line length 256), mutate in memory, write it back. Missing-file (`ENOENT`) is treated as an empty set. No locking — callers serialize via `state->lock` when writes race with other handlers.
