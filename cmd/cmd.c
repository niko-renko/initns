#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/kd.h>
#include <linux/sched.h>
#include <linux/vt.h>

#include <sys/mount.h>
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

static void clone_tar(const char *tar, const char *dest) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0) {
        if (waitpid(pid, NULL, 0) == -1 && errno != ECHILD)
            die("waitpid");
        return;
    }
    clean_fds();
    execl("/bin/tar", "tar", "xf", tar, "--strip-components=1", "-C", dest,
          (char *)NULL);
    die("execl tar");
}

static void clone_rm(const char *path) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0) {
        if (waitpid(pid, NULL, 0) == -1 && errno != ECHILD)
            die("waitpid");
        return;
    }
    clean_fds();
    execl("/bin/rm", "rm", "-rf", path, (char *)NULL);
    die("execl rm");
}

static void clone_init(int cgroup, const char *name) {
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
        return;
    clean_fds();

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
    clone_tar(image, rootfs);
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
        rm_cgroup(state->instance);
    }
    strcpy(state->instance, name);
    pthread_mutex_unlock(&state->lock);

    write(out, OK, strlen(OK));
    stop_ctl();

    int cgroup = new_cgroup(name);
    clone_init(cgroup, name);
    close(cgroup);
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

        int in = open(instances, O_RDONLY);
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
    rm_cgroup(state->instance);
    state->instance[0] = '\0';
    pthread_mutex_unlock(&state->lock);
    write(out, OK, strlen(OK));
}

static void accept_cmd(int out, char *line, int n) {
    line[n] = '\0';
    char *nl = strchr(line, '\n');
    if (nl)
        *nl = '\0';
    char *cmd = strtok(line, " ");
    char *arg = strtok(NULL, " ");
    char *arg2 = strtok(NULL, " ");
    int valid = 0;

    if (!cmd || !arg)
        goto syntax;

    if (strcmp(cmd, "new") == 0 && arg2) {
        cmd_new(out, arg, arg2);
        valid = 1;
    }
    if (strcmp(cmd, "rm") == 0) {
        cmd_rm(out, arg);
        valid = 1;
    }
    if (strcmp(cmd, "run") == 0) {
        cmd_run(out, arg);
        valid = 1;
    }
    if (strcmp(cmd, "stop") == 0) {
        cmd_stop(out, arg);
        valid = 1;
    }
    if (strcmp(cmd, "ls") == 0) {
        cmd_ls(out, arg);
        valid = 1;
    }

syntax:
    if (!valid)
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
