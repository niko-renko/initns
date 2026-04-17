#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../common.h"
#include "state.h"

static pthread_key_t state_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void create_key(void) { pthread_key_create(&state_key, NULL); }

State *init_state(void) {
    State *state = malloc(sizeof(State));
    if (pthread_mutex_init(&state->lock, NULL) != 0)
        die("mutex_init");
    state->ctl = 0;
    state->container = 0;
    state->instance = malloc(256);
    state->instance[0] = '\0';
    return state;
}

void set_state(State *state) {
    pthread_once(&key_once, create_key);
    pthread_setspecific(state_key, state);
}

State *get_state(void) { return pthread_getspecific(state_key); }