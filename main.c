#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cgroup/cgroup.h"
#include "cmd/cmd.h"
#include "common.h"
#include "kbd/kbd.h"
#include "state/state.h"

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

    init_cgroup();
    spawn_kbd();
    spawn_sock_cmd();

    for (;;)
        pause();
}
