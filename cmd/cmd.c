#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/kd.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/vt.h>

#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "../cgroup/cgroup.h"
#include "../common.h"
#include "../ctl/ctl.h"
#include "../set/set.h"
#include "../state/state.h"

static char *OK = "ok\n";
static char *ERR = "error\n";
static char *SYNTAX = "syntax\n";

static void clone_tar_extract(const char *tar, const char *dest) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0) {
        if (waitpid(pid, NULL, 0) == -1)
            die("waitpid");
        return;
    }
    execl("/bin/tar", "tar", "xf", tar, "--strip-components=1",
          "--numeric-owner", "--xattrs", "--xattrs-include=*", "--acls", "-C",
          dest, (char *)NULL);
    die("execl tar");
}

static void clone_tar_create(const char *image, const char *src) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0) {
        if (waitpid(pid, NULL, 0) == -1)
            die("waitpid");
        return;
    }
    execl("/bin/tar", "tar", "cf", image, "--numeric-owner", "--xattrs",
          "--acls", "-C", src, ".", (char *)NULL);
    die("execl tar");
}

static void clone_tar_create_onefs(const char *image, const char *src,
                                   const char *exclude) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0) {
        if (waitpid(pid, NULL, 0) == -1)
            die("waitpid");
        return;
    }
    execl("/bin/tar", "tar", "cf", image, "--one-file-system",
          "--numeric-owner", "--xattrs", "--acls", exclude, "-C", src, ".",
          (char *)NULL);
    die("execl tar");
}

static void clone_rm(const char *path) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0) {
        if (waitpid(pid, NULL, 0) == -1)
            die("waitpid");
        return;
    }
    execl("/bin/rm", "rm", "-rf", path, (char *)NULL);
    die("execl rm");
}

static pid_t clone_init(int cgroup, const char *name) {
    char rootfs[PATH_MAX];
    snprintf(rootfs, PATH_MAX, "%s/rootfs/%s", ROOT, name);
    char rootfsmnt[PATH_MAX];
    snprintf(rootfsmnt, PATH_MAX, "%s/mnt", rootfs);

    struct clone_args args;
    memset(&args, 0, sizeof(args));
    args.flags =
        CLONE_INTO_CGROUP | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWCGROUP;
    args.exit_signal = SIGCHLD;
    args.cgroup = cgroup;
    pid_t pid = syscall(SYS_clone3, &args, sizeof(args));
    if (pid < 0)
        die("clone");
    if (pid > 0)
        return pid;

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        die("mount MS_PRIVATE failed");

    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0)
        die("bind /mnt/newroot");

    if (syscall(SYS_pivot_root, rootfs, rootfsmnt) < 0)
        die("pivot_root");

    if (umount2("/mnt", MNT_DETACH) < 0)
        die("umount2");

    execl("/sbin/init", "init", (char *)NULL);
    die("execl init");
}

static void cmd_new(int out, char *name, char *image_name) {
    char instances[PATH_MAX];
    snprintf(instances, PATH_MAX, "%s/instances", ROOT);
    char rootfs[PATH_MAX];
    snprintf(rootfs, PATH_MAX, "%s/rootfs/%s", ROOT, name);
    char image[PATH_MAX];
    snprintf(image, PATH_MAX, "%s/images/%s", ROOT, image_name);

    if (file_contains(instances, name) || access(image, F_OK) != 0) {
        write(out, ERR, strlen(ERR));
        return;
    }

    file_add(instances, name);
    mkdir(rootfs, 0755);
    clone_tar_extract(image, rootfs);
    sync();
    write(out, OK, strlen(OK));
}

static void cmd_commit(int out, char *name, char *image_name) {
    char instances[PATH_MAX];
    snprintf(instances, PATH_MAX, "%s/instances", ROOT);
    char rootfs[PATH_MAX];
    snprintf(rootfs, PATH_MAX, "%s/rootfs/%s", ROOT, name);
    char image[PATH_MAX];
    snprintf(image, PATH_MAX, "%s/images/%s", ROOT, image_name);

    if (!file_contains(instances, name) || access(image, F_OK) == 0) {
        write(out, ERR, strlen(ERR));
        return;
    }

    clone_tar_create(image, rootfs);
    sync();
    write(out, OK, strlen(OK));
}

static void cmd_seed(int out, char *source_dir, char *image_name) {
    char image[PATH_MAX];
    snprintf(image, PATH_MAX, "%s/images/%s", ROOT, image_name);

    struct stat st;
    if (access(image, F_OK) == 0 || stat(source_dir, &st) < 0 ||
        !S_ISDIR(st.st_mode)) {
        write(out, ERR, strlen(ERR));
        return;
    }

    // Archive members from `tar -C src .` are "./<path-relative-to-src>".
    // To exclude ROOT, build "./<ROOT-relative-to-src>". If src isn't an
    // ancestor of ROOT, the pattern is inert (no archive member matches).
    size_t slen = strlen(source_dir);
    while (slen > 1 && source_dir[slen - 1] == '/')
        slen--;
    const char *rel = ROOT;
    if (strncmp(ROOT, source_dir, slen) == 0 &&
        (slen == 1 || ROOT[slen] == '/' || ROOT[slen] == '\0')) {
        rel = ROOT + slen;
        if (*rel == '/')
            rel++;
    }
    char exclude[PATH_MAX];
    snprintf(exclude, PATH_MAX, "--exclude=./%s", rel);

    clone_tar_create_onefs(image, source_dir, exclude);
    sync();
    write(out, OK, strlen(OK));
}

static void cmd_rm(int out, char *name) {
    char instances[PATH_MAX];
    snprintf(instances, PATH_MAX, "%s/instances", ROOT);
    char rootfs[PATH_MAX];
    snprintf(rootfs, sizeof(rootfs), "%s/rootfs/%s", ROOT, name);

    if (!file_contains(instances, name)) {
        write(out, ERR, strlen(ERR));
        return;
    }

    clone_rm(rootfs);
    file_remove(instances, name);
    sync();
    write(out, OK, strlen(OK));
}

static void cmd_run(int out, char *name) {
    char instances[PATH_MAX];
    snprintf(instances, PATH_MAX, "%s/instances", ROOT);

    if (!file_contains(instances, name)) {
        write(out, ERR, strlen(ERR));
        return;
    }

    State *state = get_state();
    pthread_mutex_lock(&state->lock);
    // This instance is running
    if (strcmp(name, state->instance) == 0) {
        set_frozen_cgroup(name, 0);
        pthread_mutex_unlock(&state->lock);
        write(out, OK, strlen(OK));
        stop_ctl();
        return;
    }
    // Another instance is running
    if (state->instance[0] != '\0') {
        sync();
        kill_cgroup(state->instance);
        if (waitpid(state->container, NULL, 0) == -1)
            die("waitpid container");
        state->container = 0;
        rm_cgroup(state->instance);
    }

    int cgroup = new_cgroup(name);
    pid_t container = clone_init(cgroup, name);
    close(cgroup);
    strcpy(state->instance, name);
    state->container = container;
    pthread_mutex_unlock(&state->lock);

    write(out, OK, strlen(OK));
    stop_ctl();
}

static void cmd_ls(int out, char *type) {
    if (strcmp(type, "image") == 0) {
        char images[PATH_MAX];
        snprintf(images, PATH_MAX, "%s/images", ROOT);

        DIR *dir = opendir(images);
        if (!dir)
            die("images opendir");

        struct dirent *de;
        int first = 1;
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            if (!first)
                write(out, "\n", 1);
            first = 0;
            write(out, de->d_name, strlen(de->d_name));
        }

        closedir(dir);
        return;
    }
    if (strcmp(type, "instance") == 0) {
        int n;
        char buf[4096];
        char instances[PATH_MAX];
        snprintf(instances, PATH_MAX, "%s/instances", ROOT);

        int in = open(instances, O_RDONLY | O_CLOEXEC);
        if (in < 0)
            die("instances open");

        while ((n = read(in, buf, sizeof(buf))) > 0)
            write(out, buf, n);
        close(in);
        return;
    }

    write(out, SYNTAX, strlen(SYNTAX));
}

static void cmd_stop(int out, char *name) {
    char instances[PATH_MAX];
    snprintf(instances, PATH_MAX, "%s/instances", ROOT);

    if (!file_contains(instances, name)) {
        write(out, ERR, strlen(ERR));
        return;
    }

    State *state = get_state();
    pthread_mutex_lock(&state->lock);
    if (strcmp(name, state->instance) != 0) {
        write(out, ERR, strlen(ERR));
        pthread_mutex_unlock(&state->lock);
        return;
    }
    sync();
    kill_cgroup(state->instance);
    if (waitpid(state->container, NULL, 0) == -1)
        die("waitpid container");
    state->container = 0;
    rm_cgroup(state->instance);
    state->instance[0] = '\0';
    pthread_mutex_unlock(&state->lock);
    write(out, OK, strlen(OK));
}

// Graceful host reboot/poweroff. The running container's systemd is signaled
// with SIGRTMIN+5 (reboot.target) or SIGRTMIN+4 (poweroff.target) so it can
// run its own shutdown jobs (unmount, journal flush, ACPI _PTS via device
// .shutdown callbacks). Only after the container has exited do we sync and
// invoke reboot(2) ourselves.
//
// Why this matters on AMD AM5: `reboot -f` from a host-context shell skips
// container teardown, leaving the platform in a post-S3-resume state when the
// kernel walks its reboot-method list. ACPI reset fails, the kernel falls
// through to writing 0x06 to port 0xCF9, and on MSI X870 boards that
// hard-reset path after S3 trips memory training failure -> auto CMOS reset.
static void cmd_reboot(int out, int how) {
    State *state = get_state();
    pthread_mutex_lock(&state->lock);
    pid_t container = state->container;
    char instance[256];
    instance[0] = '\0';
    if (state->instance[0])
        strncpy(instance, state->instance, sizeof(instance) - 1);
    pthread_mutex_unlock(&state->lock);

    int reaped = 0;
    if (container > 0 && instance[0] != '\0') {
        // Thaw if frozen, otherwise systemd can't run its shutdown target.
        set_frozen_cgroup(instance, 0);

        int sig = (how == RB_POWER_OFF) ? SIGRTMIN + 4 : SIGRTMIN + 5;
        if (kill(container, sig) == 0) {
            // Up to 30s for systemd to walk shutdown.target.
            for (int i = 0; i < 300; i++) {
                int r = waitpid(container, NULL, WNOHANG);
                if (r == container) {
                    reaped = 1;
                    break;
                }
                if (r < 0)
                    break;
                struct timespec ts = {0, 100L * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
        }

        // Fallback: SIGKILL via cgroup.kill if systemd didn't exit in time.
        if (!reaped) {
            kill_cgroup(instance);
            if (waitpid(container, NULL, 0) > 0)
                reaped = 1;
        }
        rm_cgroup(instance);

        pthread_mutex_lock(&state->lock);
        state->container = 0;
        state->instance[0] = '\0';
        pthread_mutex_unlock(&state->lock);
    }

    write(out, OK, strlen(OK));
    write(out, "\n\n", 2);
    fsync(out);

    // Drain pending writes. Two syncs because the second waits for the
    // writeback the first kicked off (relevant for the Samsung simple-suspend
    // NVMe quirk that defers FUA flushes).
    sync();
    sync();
    reboot(how);
    die("reboot");
}

static void cmd_help(int out) {
    static const char help[] =
        "new <name> <image>      create instance from image\n"
        "seed <src> <image>      create image from one filesystem tree\n"
        "commit <name> <image>   snapshot instance to image\n"
        "run <name>              start or resume instance\n"
        "stop <name>             kill instance\n"
        "rm <name>               delete instance\n"
        "ls image | instance     list images or instances\n"
        "reboot                  graceful host reboot\n"
        "poweroff                graceful host poweroff\n"
        "help                    this help";
    write(out, help, sizeof(help) - 1);
}

static void accept_cmd(int out, char *line, int n) {
    line[n] = '\0';
    char *nl = strchr(line, '\n');
    if (nl)
        *nl = '\0';

    char *cmd = strtok(line, " ");
    char *arg = strtok(NULL, " ");
    char *arg2 = strtok(NULL, " ");

    if (!cmd)
        write(out, SYNTAX, strlen(SYNTAX));
    else if (strcmp(cmd, "help") == 0)
        cmd_help(out);
    else if (strcmp(cmd, "reboot") == 0)
        cmd_reboot(out, RB_AUTOBOOT);
    else if (strcmp(cmd, "poweroff") == 0)
        cmd_reboot(out, RB_POWER_OFF);
    else if (!arg)
        write(out, SYNTAX, strlen(SYNTAX));
    else if (strcmp(cmd, "new") == 0 && arg2)
        cmd_new(out, arg, arg2);
    else if (strcmp(cmd, "commit") == 0 && arg2)
        cmd_commit(out, arg, arg2);
    else if (strcmp(cmd, "seed") == 0 && arg2)
        cmd_seed(out, arg, arg2);
    else if (strcmp(cmd, "rm") == 0)
        cmd_rm(out, arg);
    else if (strcmp(cmd, "run") == 0)
        cmd_run(out, arg);
    else if (strcmp(cmd, "stop") == 0)
        cmd_stop(out, arg);
    else if (strcmp(cmd, "ls") == 0)
        cmd_ls(out, arg);
    else
        write(out, SYNTAX, strlen(SYNTAX));

    write(out, "\n\n", 2);
    fsync(out);
}

void cmd(int in, int out) {
    char buf[256];
    int n;

    while ((n = read(in, buf, sizeof(buf) - 1)) > 0)
        accept_cmd(out, buf, n);
}
