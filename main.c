#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cgroup/cgroup.h"
#include "cmd/cmd.h"
#include "common.h"
#include "kbd/kbd.h"
#include "state/state.h"

static void reap(int sig) {
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved;
}

static void install_reaper(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
        die("sigaction SIGCHLD");
}

int main(void) {
    State *state = init_state();
    set_state(state);

    char images[PATH_MAX];
    snprintf(images, PATH_MAX, "%s/images", ROOT);
    char rootfs[PATH_MAX];
    snprintf(rootfs, PATH_MAX, "%s/rootfs", ROOT);

    if (mkdir(ROOT, 0755) == -1)
        if (errno != EEXIST)
            die("ROOT mkdir");
    if (mkdir(images, 0755) == -1)
        if (errno != EEXIST)
            die("ROOT mkdir");
    if (mkdir(rootfs, 0755) == -1)
        if (errno != EEXIST)
            die("ROOT mkdir");

    install_reaper();
    init_cgroup();
    spawn_kbd();
    spawn_sock_cmd();

    for (;;)
        pause();
}
