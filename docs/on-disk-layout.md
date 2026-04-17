# On-disk and runtime layout

All paths are set in `common.h` and `cgroup/cgroup.h`; there is no configuration file.

## `/var/lib/initns/` (`ROOT`)

```
/var/lib/initns/
├── log                       # daemon stdout+stderr, append-only
├── instances                 # one instance name per line, newline-terminated
├── images/
│   └── <image>.tar[.gz]      # container templates; referenced by filename in `initns new`
└── rootfs/
    └── <instance>/           # extracted rootfs, one dir per instance
        ├── sbin/init         # required: what the container execs
        ├── mnt/              # pivot_root puts the old root here, then it's detached
        └── ...               # contents of the image (after --strip-components=1)
```

`instances` is a plain text file managed by `set/set.c`: `file_add`, `file_remove`, `file_contains`. It is the source of truth for "does this instance exist"; the rootfs directory is only checked indirectly via `rm`.

## `/run/initns.sock`

Unix domain socket, `AF_UNIX` / `SOCK_STREAM`, created by the daemon on start. The daemon `unlink`s any stale socket before `bind` (`cmd/sock_cmd.c:40`). See `@protocol.md`.

## `/sys/fs/cgroup/initns/` (`CGROUP_ROOT + CGROUP_NAME`)

```
/sys/fs/cgroup/                  # cgroup v2 mount (the daemon mounts it if absent)
└── initns/                      # parent cgroup, created on daemon start
    └── <instance>/              # created per `run`, removed on `stop` or preempt
        ├── cgroup.freeze        # written "1"/"0" by set_frozen_cgroup()
        ├── cgroup.kill          # written "1" by kill_cgroup()
        └── ... (standard cgroup v2 files)
```

Only one instance runs at a time. Starting a second instance preempts the first (kill + rm). See `@subsystems/cgroup.md`.

## Device files touched

| Path             | Used by         | Purpose |
|---               |---              |---|
| `/dev/input/`    | `kbd/kbd.c`     | inotify watch for new keyboards |
| `/dev/input/event*` | `kbd/*`      | read `input_event` stream, classify via `EVIOCGBIT` |
| `/dev/tty0`      | `ctl/vt.c`      | `VT_ACTIVATE`, `VT_WAITACTIVE` for VT switching |
| `/dev/tty63`     | `ctl/ctl.c`, `ctl/vt.c` | controlling TTY for the host shell; `VT_SETMODE` target |
