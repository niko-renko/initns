#ifndef COMMON_H
#define COMMON_H

#define ROOT "/var/lib/initns"
#define SOCK_PATH "/run/initns.sock"

void die(const char *) __attribute__((noreturn));
void clean_fds(void);

#endif
