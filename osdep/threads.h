#ifndef MP_OSDEP_THREADS_H_
#define MP_OSDEP_THREADS_H_

#include <pthread.h>
#include <inttypes.h>

#include "misc/mp_assert.h"

#include "config.h"

#if HAVE_BSD_THREAD_NAME && (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread_np.h>
#endif

// Helper to reduce boiler plate.
int mpthread_mutex_init_recursive(pthread_mutex_t *mutex);

// Set thread name (for debuggers).
void mpthread_set_name(const char *name);

int mp_ptwrap_check(const char *file, int line, int res);
int mp_ptwrap_mutex_init(const char *file, int line, pthread_mutex_t *m,
                         const pthread_mutexattr_t *attr);
int mp_ptwrap_mutex_trylock(const char *file, int line, pthread_mutex_t *m);

#ifdef MP_PTHREAD_DEBUG

// pthread debugging wrappers. Technically, this is undefined behavior, because
// you are not supposed to define any symbols that clash with reserved names.
// Other than that, they should be fine.

// Note: dmpv normally never checks pthread error return values of certain
//       functions that  should never fail. It does so because these cases would
//       be undefined behavior anyway (such as double-frees etc.). However,
//       since there are no good pthread debugging tools, these wrappers are
//       provided for the sake of debugging. They crash on unexpected errors.
//
//       Technically, pthread_cond/mutex_init() can fail with ENOMEM. We don't
//       really respect this for normal/recursive mutex types, as due to the
//       existence of static initializers, no sane implementation could actually
//       require allocating memory.

#define MP_PTWRAP(fn, ...) \
    mp_ptwrap_check(__FILE__, __LINE__, (fn)(__VA_ARGS__))

// ISO C defines that all standard functions can be macros, except undef'ing
// them is allowed and must make the "real" definitions available. (Whatever.)
#undef pthread_cond_init
#undef pthread_cond_destroy
#undef pthread_cond_broadcast
#undef pthread_cond_signal
#undef pthread_cond_wait
#undef pthread_cond_timedwait
#undef pthread_detach
#undef pthread_join
#undef pthread_mutex_destroy
#undef pthread_mutex_lock
#undef pthread_mutex_trylock
#undef pthread_mutex_unlock

#define pthread_cond_init(...)      MP_PTWRAP(pthread_cond_init, __VA_ARGS__)
#define pthread_cond_destroy(...)   MP_PTWRAP(pthread_cond_destroy, __VA_ARGS__)
#define pthread_cond_broadcast(...) MP_PTWRAP(pthread_cond_broadcast, __VA_ARGS__)
#define pthread_cond_signal(...)    MP_PTWRAP(pthread_cond_signal, __VA_ARGS__)
#define pthread_cond_wait(...)      MP_PTWRAP(pthread_cond_wait, __VA_ARGS__)
#define pthread_cond_timedwait(...) MP_PTWRAP(pthread_cond_timedwait, __VA_ARGS__)
#define pthread_detach(...)         MP_PTWRAP(pthread_detach, __VA_ARGS__)
#define pthread_join(...)           MP_PTWRAP(pthread_join, __VA_ARGS__)
#define pthread_mutex_destroy(...)  MP_PTWRAP(pthread_mutex_destroy, __VA_ARGS__)
#define pthread_mutex_lock(...)     MP_PTWRAP(pthread_mutex_lock, __VA_ARGS__)
#define pthread_mutex_unlock(...)   MP_PTWRAP(pthread_mutex_unlock, __VA_ARGS__)

#define pthread_mutex_init(...) \
    mp_ptwrap_mutex_init(__FILE__, __LINE__, __VA_ARGS__)

#define pthread_mutex_trylock(...) \
    mp_ptwrap_mutex_trylock(__FILE__, __LINE__, __VA_ARGS__)

#endif

enum mp_mutex_type {
    MP_MUTEX_NORMAL = 0,
    MP_MUTEX_RECURSIVE,
};

#define mp_mutex_init(mutex) \
    mp_mutex_init_type(mutex, MP_MUTEX_NORMAL)

#define mp_mutex_init_type(mutex, mtype) \
    mp_assert(!mp_mutex_init_type_internal(mutex, mtype))

#endif

#pragma once

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

#include "common/common.h"
#include "config.h"
#include "timer.h"

typedef pthread_cond_t  mp_cond;
typedef pthread_mutex_t mp_mutex;
typedef pthread_mutex_t mp_static_mutex;
typedef pthread_once_t  mp_once;
typedef pthread_t       mp_thread;

#define MP_STATIC_COND_INITIALIZER PTHREAD_COND_INITIALIZER
#define MP_STATIC_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define MP_STATIC_ONCE_INITIALIZER PTHREAD_ONCE_INIT

static inline int mp_mutex_init_type_internal(mp_mutex *mutex, enum mp_mutex_type mtype)
{
    int mutex_type;
    switch (mtype) {
    case MP_MUTEX_RECURSIVE:
        mutex_type = PTHREAD_MUTEX_RECURSIVE;
        break;
    case MP_MUTEX_NORMAL:
    default:
#ifndef NDEBUG
        mutex_type = PTHREAD_MUTEX_ERRORCHECK;
#else
        mutex_type = PTHREAD_MUTEX_DEFAULT;
#endif
        break;
    }

    int ret = 0;
    pthread_mutexattr_t attr;
    ret = pthread_mutexattr_init(&attr);
    if (ret != 0)
        return ret;

    pthread_mutexattr_settype(&attr, mutex_type);
    ret = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    return ret;
}

#define mp_mutex_destroy    pthread_mutex_destroy
#define mp_mutex_lock       pthread_mutex_lock
#define mp_mutex_trylock    pthread_mutex_trylock
#define mp_mutex_unlock     pthread_mutex_unlock

#define mp_cond_init(cond)  pthread_cond_init(cond, NULL)
#define mp_cond_destroy     pthread_cond_destroy
#define mp_cond_broadcast   pthread_cond_broadcast
#define mp_cond_signal      pthread_cond_signal
#define mp_cond_wait        pthread_cond_wait

static inline int mp_cond_timedwait(mp_cond *cond, mp_mutex *mutex, int64_t timeout)
{
    timeout = MPMAX(0, timeout);
    // consider anything above 1000 days as infinity
    if (timeout > MP_TIME_S_TO_NS(1000 * 24 * 60 * 60))
        return pthread_cond_wait(cond, mutex);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout / MP_TIME_S_TO_NS(1);
    ts.tv_nsec += timeout % MP_TIME_S_TO_NS(1);
    if (ts.tv_nsec >= MP_TIME_S_TO_NS(1)) {
        ts.tv_nsec -= MP_TIME_S_TO_NS(1);
        ts.tv_sec++;
    }

    return pthread_cond_timedwait(cond, mutex, &ts);
}

static inline int mp_cond_timedwait_until(mp_cond *cond, mp_mutex *mutex, int64_t until)
{
    return mp_cond_timedwait(cond, mutex, until - mp_time_ns());
}

#define mp_exec_once pthread_once

#define MP_THREAD_VOID void *
#define MP_THREAD_RETURN() return NULL

#define mp_thread_create(t, f, a) pthread_create(t, NULL, f, a)
#define mp_thread_join(t)         pthread_join(t, NULL)
#define mp_thread_self            pthread_self
#define mp_thread_equal           pthread_equal

static inline void mp_thread_set_name(const char *name)
{
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
#if HAVE_GLIBC_THREAD_NAME
    if (pthread_setname_np(pthread_self(), name) == ERANGE) {
        char tname[16]; // glibc-checked kernel limit
        strncpy(tname, name, sizeof(tname) - 1);
        tname[sizeof(tname) - 1] = '\0'; // Ensure null-termination
        pthread_setname_np(pthread_self(), tname);
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#elif HAVE_BSD_THREAD_NAME
#ifdef __NetBSD__
    pthread_setname_np(pthread_self(), name, NULL);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    pthread_set_name_np(pthread_self(), name);
#endif
#endif
}

static inline int64_t mp_thread_cpu_time_ns(mp_thread thread)
{
#if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(_POSIX_THREAD_CPUTIME)
    clockid_t id;
    struct timespec ts;
    if (pthread_getcpuclockid(thread, &id) == 0 && clock_gettime(id, &ts) == 0)
        return MP_TIME_S_TO_NS(ts.tv_sec) + ts.tv_nsec;
#endif
    return 0;
}
