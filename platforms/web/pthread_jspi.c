// Web/SDK boundary only. Emscripten 6.0.9's pthread_create failure branch
// unlinks the new pthread but does not free its stack/TLS allocation. Wait for
// that rollback to return before freeing a failed asynchronous creation.
#include <emscripten.h>
#include <assert.h>
#include <pthread.h>

struct create_state { pthread_t failed_thread; };
static _Thread_local struct create_state *current_create;

EMSCRIPTEN_KEEPALIVE void *krkr2_pthread_create_state(void) {
    assert(current_create && "pthread_create must use the Web linker wrappers");
    return current_create;
}

EMSCRIPTEN_KEEPALIVE void krkr2_pthread_create_failed(
    struct create_state *state, pthread_t thread) {
    state->failed_thread = thread;
}

extern void _emscripten_thread_free_data(pthread_t thread);

typedef int (*create_fn)(pthread_t *, const pthread_attr_t *,
                         void *(*)(void *), void *);

static int create(create_fn original, pthread_t *thread,
                  const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
    struct create_state state = {0};
    struct create_state *previous = current_create;
    current_create = &state;
    int result = original(thread, attr, entry, arg);
    if (result && state.failed_thread) {
        _emscripten_thread_free_data(state.failed_thread);
    }
    current_create = previous;
    return result;
}

// libc++, C11 threads and the sanitizer interceptor can use different aliases.
// Nested wrappers are safe: only the innermost wrapper owns the failed block.
#define WRAP_CREATE(name)                                                       \
    extern int __real_##name(pthread_t *, const pthread_attr_t *,                \
                             void *(*)(void *), void *);                         \
    int __wrap_##name(pthread_t *thread, const pthread_attr_t *attr,              \
                      void *(*entry)(void *), void *arg) {                       \
        return create(__real_##name, thread, attr, entry, arg);                  \
    }

WRAP_CREATE(pthread_create)
WRAP_CREATE(__pthread_create)
WRAP_CREATE(emscripten_builtin_pthread_create)
