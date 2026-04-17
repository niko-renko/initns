#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

#include "../cgroup/cgroup.h"
#include "../common.h"
#include "../ctl/ctl.h"
#include "../state/state.h"

#define INPUT_DIR "/dev/input"
#define EVENT_PREFIX "event"

#define MOD_LCTRL 0x1
#define MOD_RCTRL 0x2
#define MOD_LALT 0x4
#define MOD_RALT 0x8
#define MOD_CTRL (MOD_LCTRL | MOD_RCTRL)
#define MOD_ALT (MOD_LALT | MOD_RALT)

#define DEBOUNCE_NS 200000000L

typedef struct kbd_dev {
    int fd;
    uint8_t mods;
    char path[64];
    struct kbd_dev *next;
} kbd_dev;

static kbd_dev *devices;
static int epfd;
static int inotify_fd;
static int inotify_tag;
static long last_fire_ns;

static int is_event_device(const char *name) {
    return strncmp(name, EVENT_PREFIX, strlen(EVENT_PREFIX)) == 0;
}

static int classify_kbd(int fd) {
    unsigned long evbit[(EV_MAX + 7) / 8] = {0};
    unsigned long keybit[(KEY_MAX + 7) / 8] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0)
        return 0;
    if (!(evbit[EV_KEY / 8] & (1 << (EV_KEY % 8))) ||
        !(evbit[EV_SYN / 8] & (1 << (EV_SYN % 8))))
        return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0)
        return 0;

    int count = 0;
    for (int code = 1; code <= 255; code++) {
        if (keybit[code / 8] & (1 << (code % 8)))
            count++;
        if (count > 15)
            return 1;
    }
    return 0;
}

static uint8_t code_to_mod(int code) {
    switch (code) {
    case KEY_LEFTCTRL:
        return MOD_LCTRL;
    case KEY_RIGHTCTRL:
        return MOD_RCTRL;
    case KEY_LEFTALT:
        return MOD_LALT;
    case KEY_RIGHTALT:
        return MOD_RALT;
    }
    return 0;
}

static uint8_t query_mods(int fd) {
    unsigned long keys[(KEY_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] =
        {0};
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
        return 0;
    uint8_t m = 0;
#define HELD(c)                                                                \
    (keys[(c) / (8 * sizeof(long))] & (1UL << ((c) % (8 * sizeof(long)))))
    if (HELD(KEY_LEFTCTRL))
        m |= MOD_LCTRL;
    if (HELD(KEY_RIGHTCTRL))
        m |= MOD_RCTRL;
    if (HELD(KEY_LEFTALT))
        m |= MOD_LALT;
    if (HELD(KEY_RIGHTALT))
        m |= MOD_RALT;
#undef HELD
    return m;
}

static uint8_t global_mods(void) {
    uint8_t m = 0;
    for (kbd_dev *d = devices; d; d = d->next)
        m |= d->mods;
    return m;
}

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static void on_ctl(void) {
    start_ctl();

    State *state = get_state();
    pthread_mutex_lock(&state->lock);
    if (state->instance[0] != '\0')
        set_frozen_cgroup(state->instance, 1);
    pthread_mutex_unlock(&state->lock);
}

static kbd_dev *find_device(const char *path) {
    for (kbd_dev *d = devices; d; d = d->next)
        if (strcmp(d->path, path) == 0)
            return d;
    return NULL;
}

static void remove_device(kbd_dev *dev) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, dev->fd, NULL);
    close(dev->fd);

    kbd_dev **pp = &devices;
    while (*pp && *pp != dev)
        pp = &(*pp)->next;
    if (*pp == dev)
        *pp = dev->next;
    free(dev);
}

static void add_device(const char *path) {
    if (find_device(path))
        return;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    if (!classify_kbd(fd)) {
        close(fd);
        return;
    }

    kbd_dev *dev = malloc(sizeof(*dev));
    if (!dev) {
        close(fd);
        return;
    }
    dev->fd = fd;
    dev->mods = query_mods(fd);
    strncpy(dev->path, path, sizeof(dev->path) - 1);
    dev->path[sizeof(dev->path) - 1] = '\0';
    dev->next = NULL;

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = dev;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        free(dev);
        return;
    }

    dev->next = devices;
    devices = dev;
}

static void handle_evdev(kbd_dev *dev) {
    struct input_event ev;
    ssize_t n = read(dev->fd, &ev, sizeof(ev));
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return;
        remove_device(dev);
        return;
    }
    if (n == 0) {
        remove_device(dev);
        return;
    }
    if (n != (ssize_t)sizeof(ev))
        return;

    if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
        dev->mods = query_mods(dev->fd);
        return;
    }
    if (ev.type != EV_KEY)
        return;

    uint8_t bit = code_to_mod(ev.code);
    if (bit) {
        if (ev.value == 0)
            dev->mods &= ~bit;
        else
            dev->mods |= bit;
        return;
    }

    if (ev.code == KEY_J && ev.value == 1) {
        uint8_t g = global_mods();
        if ((g & MOD_CTRL) && (g & MOD_ALT)) {
            long now = now_ns();
            if (now - last_fire_ns >= DEBOUNCE_NS) {
                last_fire_ns = now;
                on_ctl();
            }
        }
    }
}

static void handle_inotify(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(inotify_fd, buf, sizeof(buf));
    if (len <= 0)
        return;
    for (char *ptr = buf; ptr < buf + len;) {
        struct inotify_event *ie = (struct inotify_event *)ptr;
        if ((ie->mask & IN_CREATE) && is_event_device(ie->name)) {
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", INPUT_DIR, ie->name);
            add_device(full);
        }
        ptr += sizeof(struct inotify_event) + ie->len;
    }
}

static void scan_existing_devices(void) {
    DIR *dir = opendir(INPUT_DIR);
    if (!dir)
        return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (!is_event_device(de->d_name))
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", INPUT_DIR, de->d_name);
        add_device(full);
    }
    closedir(dir);
}

static void *kbd(void *arg) {
    State *state = arg;
    set_state(state);

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        die("epoll_create1");

    inotify_fd = inotify_init1(IN_CLOEXEC);
    if (inotify_fd < 0)
        die("inotify_init1");
    if (inotify_add_watch(inotify_fd, INPUT_DIR, IN_CREATE) < 0)
        die("inotify_add_watch");

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = &inotify_tag;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &ev) < 0)
        die("epoll_ctl inotify");

    scan_existing_devices();

    struct epoll_event evs[16];
    for (;;) {
        int n = epoll_wait(epfd, evs, 16, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("epoll_wait");
        }
        for (int i = 0; i < n; i++) {
            if (evs[i].data.ptr == &inotify_tag)
                handle_inotify();
            else
                handle_evdev(evs[i].data.ptr);
        }
    }
    return NULL;
}

void spawn_kbd(void) {
    pthread_t kbd_t;
    if (pthread_create(&kbd_t, NULL, kbd, get_state()) != 0)
        die("pthread_create");
}
