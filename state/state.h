#ifndef STATE_H
#define STATE_H

typedef struct state {
    pthread_mutex_t lock;
    pid_t ctl;
    pid_t container;
    char *instance;
} State;

State *init_state(void);
void set_state(State *);
State *get_state(void);

#endif