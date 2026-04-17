#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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

void vt_switch(int vt) {
    int tty0 = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty0 < 0)
        die("tty0 open");
    if (ioctl(tty0, VT_ACTIVATE, vt) < 0)
        die("VT_ACTIVATE");
    while (ioctl(tty0, VT_WAITACTIVE, vt) < 0) {
        if (errno == EINTR)
            continue;
        die("VT_WAITACTIVE");
        break;
    }
    close(tty0);
}

void vt_mode(int modeval) {
    int tty63 = open("/dev/tty63", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty63 < 0)
        die("tty63 open");
    struct vt_mode mode = {0};
    mode.mode = modeval;
    mode.relsig = SIGWINCH;
    mode.acqsig = SIGWINCH;
    if (ioctl(tty63, VT_SETMODE, &mode) < 0)
        die("VT_SETMODE");
    close(tty63);
}