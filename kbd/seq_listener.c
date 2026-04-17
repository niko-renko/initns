#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "../cgroup/cgroup.h"
#include "../common.h"
#include "../ctl/ctl.h"
#include "../state/state.h"

struct seq_listener_args {
    State *state;
    char device_path[PATH_MAX];
};

void on_ctl() {
    start_ctl();

    State *state = get_state();
    pthread_mutex_lock(&state->lock);
    if (state->instance[0] != '\0')
        set_frozen_cgroup(state->instance, 1);
    pthread_mutex_unlock(&state->lock);
}

static void *seq_listener(void *arg) {
    struct seq_listener_args *args = arg;
    set_state(args->state);

    char device_path[PATH_MAX];
    strncpy(device_path, args->device_path, sizeof(device_path));
    free(args);

    int fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return NULL;

    struct input_event ev;
    int ctrl = 0, alt = 0;

    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));

        if (n < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK))
	    break;
        if (n != (ssize_t)sizeof(ev) || ev.type != EV_KEY)
            continue;

        if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
            ctrl = ev.value;
        else if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
            alt = ev.value;
        else if (ev.code == KEY_J && ev.value == 1)
            if (ctrl && alt)
                on_ctl();
    }

    close(fd);
    return NULL;
}

void spawn_seq_listener(const char *devpath) {
    pthread_t seq_listener_t;
    struct seq_listener_args *args = malloc(sizeof(*args));
    if (!args)
        die("malloc");
    args->state = get_state();
    strncpy(args->device_path, devpath, sizeof(args->device_path) - 1);
    args->device_path[sizeof(args->device_path) - 1] = '\0';
    if (pthread_create(&seq_listener_t, NULL, seq_listener, args) != 0) {
        free(args);
        die("pthread_create");
    }
    pthread_detach(seq_listener_t);
}
