# initns

A lightweight Linux container management daemon written in C that provides basic container lifecycle management using Linux namespaces and cgroups.

## Why

Unlike traditional containers, these are fully unrestricted with access to all system calls and devtmpfs. The goal isn't isolation for security—it's having multiple temporary filesystems over the same kernel that I can switch between.

This lets me:

- **Experiment freely** - Try things out, dirty a filesystem, then delete it
- **Specialize environments** - Maintain separate filesystems for specific tasks or projects
- **Stay lightweight** - No VM overhead, just namespace separation over a shared kernel

## Architecture

The project produces two binaries:

- **init** - Main daemon running in the background, managing containers
- **initns** - CLI client communicating with the daemon via Unix socket

## Directory Structure

```
initns/
├── main.c           # Main daemon entry point
├── main_initns.c    # CLI client entry point
├── common.c/h       # Shared utility functions
├── Makefile         # Build configuration
├── cgroup/          # cgroup v2 management
├── cmd/             # Command processing and Unix socket server
├── ctl/             # Control terminal management
├── kbd/             # Keyboard input and hotkey handling
├── set/             # File-based set operations
└── state/           # Thread-safe global state management
```

## Components

| Component | Purpose |
|-----------|---------|
| `cmd/` | Command processing (new, rm, run, stop, ls) |
| `cgroup/` | cgroup v2 management for container isolation |
| `ctl/` | Emergency control terminal (host shell on VT63) |
| `kbd/` | Keyboard hotkey detection (Ctrl+Alt+J) |
| `set/` | File-based set operations for tracking instances |
| `state/` | Thread-safe global state management |

## Commands

| Command | Arguments | Description |
|---------|-----------|-------------|
| `new` | `<name> <image>` | Create a new container instance from a tarball image |
| `rm` | `<name>` | Remove a container instance |
| `run` | `<name>` | Start/resume a container instance |
| `stop` | `<name>` | Stop a running container |
| `ls` | `image` or `instance` | List available images or instances |

## Container Isolation

Containers are isolated using:

- `clone3()` syscall with `CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWCGROUP`, `CLONE_INTO_CGROUP`
- `pivot_root` for filesystem isolation
- cgroup v2 for process grouping, freezing, and termination

## Data Paths

| Path | Purpose |
|------|---------|
| `/var/lib/initns/images/` | Container image tarballs |
| `/var/lib/initns/rootfs/` | Extracted container root filesystems |
| `/var/lib/initns/instances` | List of created instance names |
| `/var/lib/initns/log` | Daemon log output |
| `/run/initns.sock` | Unix socket for CLI communication |
| `/sys/fs/cgroup/initns/` | Cgroup hierarchy for containers |

## Emergency Access

Press `Ctrl+Alt+J` to freeze the running container and open a host shell on VT63 for system administration.

## Build

```bash
make init     # Build the daemon
make initns   # Build the CLI client
make clean    # Remove built binaries
```
