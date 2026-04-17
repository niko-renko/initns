CC = gcc
CFLAGS = -Wall -Wextra -Wunused -O2 -D_GNU_SOURCE
COMMON = set/*.c cmd/*.c kbd/*.c state/*.c cgroup/*.c ctl/*.c common.c

init:
	$(CC) $(CFLAGS) main.c $(COMMON) -o bin/init

initns:
	$(CC) $(CFLAGS) main_initns.c $(COMMON) -o bin/initns

clean:
	rm -f bin/*
