#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

#include "../common.h"
#include "../state/state.h"

void vt_switch(int);
void vt_mode(int);

static pid_t clone_shell(void) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0)
        return pid;
    clean_fds();

    if (setsid() < 0)
        die("setsid");

    int tty63 = open("/dev/tty63", O_RDWR | O_NOCTTY);
    if (tty63 < 0)
        die("tty63 open");
    dup2(tty63, STDOUT_FILENO);
    dup2(tty63, STDERR_FILENO);
    if (ioctl(tty63, TIOCSCTTY, (void *)1) < 0)
        die("TIOCSCTTY");
    if (ioctl(tty63, KDSETMODE, KD_TEXT) < 0)
        die("KD_TEXT");
    if (ioctl(tty63, KDSKBMODE, K_UNICODE) < 0)
        die("KDSKBMODE");

    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("HOME", "/root", 1);
    execl("/bin/bash", "bash", (char *)NULL);
    die("execl bash");
    return 0;
}

static void clone_pkill(int sid) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid > 0) {
        if (waitpid(pid, NULL, 0) == -1)
            die("waitpid");
        return;
    }
    clean_fds();
    char ssid[12];
    sprintf(ssid, "%d", sid);
    execl("/bin/pkill", "pkill", "-9", "-s", ssid, (char *)NULL);
    die("execl pkill");
}

void start_ctl(void) {
    State *state = get_state();
    pthread_mutex_lock(&state->lock);
    if (state->ctl) {
        clone_pkill(state->ctl);
        if (waitpid(state->ctl, NULL, 0) == -1)
            die("waitpid ctl");
    }
    state->ctl = clone_shell();
    pthread_mutex_unlock(&state->lock);

    vt_mode(VT_PROCESS);
    vt_switch(63);
}

void stop_ctl(void) {
    State *state = get_state();
    pthread_mutex_lock(&state->lock);
    if (state->ctl) {
        clone_pkill(state->ctl);
        if (waitpid(state->ctl, NULL, 0) == -1)
            die("waitpid ctl");
    }
    state->ctl = 0;
    pthread_mutex_unlock(&state->lock);

    vt_mode(VT_AUTO);
    vt_switch(1);
}