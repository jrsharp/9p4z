# Portable BBS Architecture

## Goal

Share core BBS/chat logic between Zephyr RTOS and Unix implementations while abstracting platform-specific APIs (threading, time, I/O).

## Strategy: Abstraction Layers

```
┌─────────────────────────────────────────────────┐
│         Application Layer (Shared)              │
│  - BBS room management (bbs.c)                  │
│  - Chat ring buffer (chat.c)                    │
│  - Message formatting                           │
│  - User management                              │
└────────────┬────────────────────────────────────┘
             │
┌────────────▼────────────────────────────────────┐
│      Platform Abstraction Layer (PAL)           │
│  - Threading (pal_thread.h)                     │
│  - Mutex/Semaphore (pal_sync.h)                 │
│  - Time (pal_time.h)                            │
│  - Memory (pal_mem.h)                           │
└────────────┬────────────────────────────────────┘
             │
        ┌────┴────┐
        ▼         ▼
  ┌─────────┐  ┌──────────┐
  │ Zephyr  │  │  Unix    │
  │  PAL    │  │   PAL    │
  │ (K_*)   │  │(pthread) │
  └─────────┘  └──────────┘
```

## Directory Structure

```
9p4z/
├── src/
│   ├── core/              # Shared BBS logic (portable)
│   │   ├── bbs_core.c
│   │   ├── chat_core.c
│   │   └── user_core.c
│   ├── pal/               # Platform Abstraction Layer
│   │   ├── pal_thread.h
│   │   ├── pal_sync.h
│   │   ├── pal_time.h
│   │   ├── pal_mem.h
│   │   ├── zephyr/        # Zephyr implementations
│   │   │   ├── pal_thread_zephyr.c
│   │   │   ├── pal_sync_zephyr.c
│   │   │   └── pal_time_zephyr.c
│   │   └── unix/          # Unix implementations
│   │       ├── pal_thread_unix.c
│   │       ├── pal_sync_unix.c
│   │       └── pal_time_unix.c
│   ├── 9bbs/              # Zephyr-specific 9P integration
│   │   └── 9bbs.c         # Uses src/core/*
│   └── unix/              # Unix-specific server
│       └── 9pbbs_server.c # Uses src/core/*
└── include/
    └── 9p/
        └── pal/
            ├── pal_thread.h
            ├── pal_sync.h
            ├── pal_time.h
            └── pal_mem.h
```

## Platform Abstraction Layer (PAL)

### pal_thread.h - Threading Abstraction

```c
#ifndef PAL_THREAD_H
#define PAL_THREAD_H

#include <stdint.h>
#include <stdbool.h>

/* Thread handle (opaque) */
typedef struct pal_thread pal_thread_t;

/* Thread function signature */
typedef void (*pal_thread_func_t)(void *arg);

/* Thread priority levels (abstract) */
typedef enum {
    PAL_THREAD_PRIO_LOW,
    PAL_THREAD_PRIO_NORMAL,
    PAL_THREAD_PRIO_HIGH,
} pal_thread_prio_t;

/* Create and start a thread */
int pal_thread_create(pal_thread_t **thread,
                      pal_thread_func_t func,
                      void *arg,
                      const char *name,
                      size_t stack_size,
                      pal_thread_prio_t prio);

/* Wait for thread to finish */
int pal_thread_join(pal_thread_t *thread);

/* Current thread yields CPU */
void pal_thread_yield(void);

/* Sleep for milliseconds */
void pal_thread_sleep_ms(uint32_t ms);

#endif /* PAL_THREAD_H */
```

### pal_sync.h - Synchronization Primitives

```c
#ifndef PAL_SYNC_H
#define PAL_SYNC_H

#include <stdint.h>
#include <stdbool.h>

/* Mutex */
typedef struct pal_mutex pal_mutex_t;

int pal_mutex_init(pal_mutex_t *mutex);
int pal_mutex_lock(pal_mutex_t *mutex);
int pal_mutex_unlock(pal_mutex_t *mutex);
int pal_mutex_destroy(pal_mutex_t *mutex);

/* Condition variable (for blocking reads) */
typedef struct pal_cond pal_cond_t;

int pal_cond_init(pal_cond_t *cond);
int pal_cond_wait(pal_cond_t *cond, pal_mutex_t *mutex);
int pal_cond_timedwait(pal_cond_t *cond, pal_mutex_t *mutex, uint32_t timeout_ms);
int pal_cond_signal(pal_cond_t *cond);
int pal_cond_broadcast(pal_cond_t *cond);
int pal_cond_destroy(pal_cond_t *cond);

/* Semaphore */
typedef struct pal_sem pal_sem_t;

int pal_sem_init(pal_sem_t *sem, uint32_t initial_count);
int pal_sem_wait(pal_sem_t *sem);
int pal_sem_post(pal_sem_t *sem);
int pal_sem_destroy(pal_sem_t *sem);

#endif /* PAL_SYNC_H */
```

### pal_time.h - Time Functions

```c
#ifndef PAL_TIME_H
#define PAL_TIME_H

#include <stdint.h>

/* Unix timestamp (seconds since epoch) */
typedef int64_t pal_time_t;

/* Get current Unix timestamp */
pal_time_t pal_time_now(void);

/* Get uptime in milliseconds */
uint64_t pal_uptime_ms(void);

/* Format time as HH:MM:SS */
void pal_time_format(pal_time_t time, char *buf, size_t bufsize);

#endif /* PAL_TIME_H */
```

### pal_mem.h - Memory Allocation

```c
#ifndef PAL_MEM_H
#define PAL_MEM_H

#include <stddef.h>

/* Allocate memory */
void *pal_malloc(size_t size);
void *pal_calloc(size_t nmemb, size_t size);
void *pal_realloc(void *ptr, size_t size);
void pal_free(void *ptr);

/* String functions (for platforms without libc) */
size_t pal_strlen(const char *s);
char *pal_strdup(const char *s);
int pal_strcmp(const char *s1, const char *s2);
int pal_strncmp(const char *s1, const char *s2, size_t n);
char *pal_strcpy(char *dst, const char *src);
char *pal_strncpy(char *dst, const char *src, size_t n);

#endif /* PAL_MEM_H */
```

## Zephyr PAL Implementation

### pal_sync_zephyr.c

```c
#include <zephyr/kernel.h>
#include "pal/pal_sync.h"

/* Zephyr uses K_MUTEX_DEFINE or k_mutex */
struct pal_mutex {
    struct k_mutex kmutex;
};

int pal_mutex_init(pal_mutex_t *mutex)
{
    return k_mutex_init(&mutex->kmutex);
}

int pal_mutex_lock(pal_mutex_t *mutex)
{
    return k_mutex_lock(&mutex->kmutex, K_FOREVER);
}

int pal_mutex_unlock(pal_mutex_t *mutex)
{
    return k_mutex_unlock(&mutex->kmutex);
}

int pal_mutex_destroy(pal_mutex_t *mutex)
{
    /* Zephyr doesn't require explicit destroy */
    return 0;
}

/* Condition variable using k_poll */
struct pal_cond {
    struct k_poll_signal signal;
    uint32_t waiters;
};

int pal_cond_init(pal_cond_t *cond)
{
    k_poll_signal_init(&cond->signal);
    cond->waiters = 0;
    return 0;
}

int pal_cond_wait(pal_cond_t *cond, pal_mutex_t *mutex)
{
    return pal_cond_timedwait(cond, mutex, K_FOREVER);
}

int pal_cond_timedwait(pal_cond_t *cond, pal_mutex_t *mutex, uint32_t timeout_ms)
{
    struct k_poll_event events[1];

    k_poll_event_init(&events[0], K_POLL_TYPE_SIGNAL,
                     K_POLL_MODE_NOTIFY_ONLY, &cond->signal);

    cond->waiters++;
    k_mutex_unlock(&mutex->kmutex);

    int ret = k_poll(events, 1, K_MSEC(timeout_ms));

    k_mutex_lock(&mutex->kmutex, K_FOREVER);
    cond->waiters--;

    if (ret == 0) {
        /* Reset signal for next waiter */
        k_poll_signal_reset(&cond->signal);
    }

    return ret;
}

int pal_cond_signal(pal_cond_t *cond)
{
    if (cond->waiters > 0) {
        k_poll_signal_raise(&cond->signal, 0);
    }
    return 0;
}

int pal_cond_broadcast(pal_cond_t *cond)
{
    /* Signal all waiters */
    while (cond->waiters > 0) {
        k_poll_signal_raise(&cond->signal, 0);
    }
    return 0;
}
```

### pal_time_zephyr.c

```c
#include <zephyr/kernel.h>
#include <time.h>
#include "pal/pal_time.h"

pal_time_t pal_time_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

uint64_t pal_uptime_ms(void)
{
    return k_uptime_get();
}

void pal_time_format(pal_time_t time, char *buf, size_t bufsize)
{
    struct tm tm;
    time_t t = (time_t)time;
    gmtime_r(&t, &tm);
    snprintf(buf, bufsize, "%02d:%02d:%02d",
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}
```

## Unix PAL Implementation

### pal_sync_unix.c

```c
#include <pthread.h>
#include <errno.h>
#include "pal/pal_sync.h"

struct pal_mutex {
    pthread_mutex_t pmutex;
};

int pal_mutex_init(pal_mutex_t *mutex)
{
    return pthread_mutex_init(&mutex->pmutex, NULL);
}

int pal_mutex_lock(pal_mutex_t *mutex)
{
    return pthread_mutex_lock(&mutex->pmutex);
}

int pal_mutex_unlock(pal_mutex_t *mutex)
{
    return pthread_mutex_unlock(&mutex->pmutex);
}

int pal_mutex_destroy(pal_mutex_t *mutex)
{
    return pthread_mutex_destroy(&mutex->pmutex);
}

struct pal_cond {
    pthread_cond_t pcond;
};

int pal_cond_init(pal_cond_t *cond)
{
    return pthread_cond_init(&cond->pcond, NULL);
}

int pal_cond_wait(pal_cond_t *cond, pal_mutex_t *mutex)
{
    return pthread_cond_wait(&cond->pcond, &mutex->pmutex);
}

int pal_cond_timedwait(pal_cond_t *cond, pal_mutex_t *mutex, uint32_t timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;

    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    int ret = pthread_cond_timedwait(&cond->pcond, &mutex->pmutex, &ts);
    return (ret == ETIMEDOUT) ? -EAGAIN : ret;
}

int pal_cond_signal(pal_cond_t *cond)
{
    return pthread_cond_signal(&cond->pcond);
}

int pal_cond_broadcast(pal_cond_t *cond)
{
    return pthread_cond_broadcast(&cond->pcond);
}
```

### pal_time_unix.c

```c
#include <time.h>
#include <sys/time.h>
#include "pal/pal_time.h"

pal_time_t pal_time_now(void)
{
    return time(NULL);
}

uint64_t pal_uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void pal_time_format(pal_time_t time, char *buf, size_t bufsize)
{
    struct tm tm;
    time_t t = (time_t)time;
    localtime_r(&t, &tm);
    snprintf(buf, bufsize, "%02d:%02d:%02d",
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}
```

## Shared Core Implementation

### chat_core.c (Portable!)

```c
#include "pal/pal_sync.h"
#include "pal/pal_time.h"
#include "pal/pal_mem.h"
#include "9p/chat_core.h"

int chat_init(struct chat_instance *chat)
{
    pal_mutex_init(&chat->lock);

    for (int i = 0; i < MAX_CHAT_ROOMS; i++) {
        pal_mutex_init(&chat->rooms[i].lock);
        pal_cond_init(&chat->rooms[i].new_msg);
        chat->rooms[i].active = false;
    }

    chat->room_count = 0;
    chat->user_count = 0;

    return 0;
}

int chat_post_message(struct chat_instance *chat, const char *room_name,
                      const char *username, const char *text)
{
    struct chat_room *room = chat_find_or_create_room(chat, room_name);
    if (!room) return -1;

    pal_mutex_lock(&room->lock);

    /* Add to ring buffer */
    uint32_t pos = room->write_pos % MAX_CHAT_MESSAGES;
    struct chat_message *msg = &room->messages[pos];

    pal_strncpy(msg->username, username, MAX_USERNAME - 1);
    pal_strncpy(msg->text, text, MAX_CHAT_MESSAGE_LEN - 1);
    msg->timestamp = pal_time_now();

    room->write_pos++;
    if (room->count < MAX_CHAT_MESSAGES) {
        room->count++;
    } else {
        room->read_pos++;
    }

    /* Wake all blocked readers */
    pal_cond_broadcast(&room->new_msg);

    pal_mutex_unlock(&room->lock);
    return 0;
}

ssize_t chat_read_messages(struct chat_instance *chat, const char *room_name,
                           const char *username, char *buf, size_t bufsize,
                           int32_t timeout_ms)
{
    struct chat_room *room = chat_find_room(chat, room_name);
    if (!room) return -1;

    struct chat_user *user = find_or_create_user(chat, username);
    if (!user) return -1;

    int room_idx = room - chat->rooms;
    uint32_t user_pos = user->read_positions[room_idx];

    pal_mutex_lock(&room->lock);

    /* Wait for new messages */
    while (user_pos >= room->write_pos) {
        int ret = pal_cond_timedwait(&room->new_msg, &room->lock, timeout_ms);
        if (ret != 0) {
            /* Timeout or error */
            pal_mutex_unlock(&room->lock);
            return 0;
        }
    }

    /* Format messages */
    size_t written = 0;
    while (user_pos < room->write_pos && written < bufsize - 100) {
        uint32_t pos = user_pos % MAX_CHAT_MESSAGES;
        struct chat_message *msg = &room->messages[pos];

        char timebuf[16];
        pal_time_format(msg->timestamp, timebuf, sizeof(timebuf));

        int n = snprintf(buf + written, bufsize - written,
                        "[%s] %s: %s\n",
                        timebuf, msg->username, msg->text);

        written += n;
        user_pos++;
    }

    user->read_positions[room_idx] = user_pos;
    pal_mutex_unlock(&room->lock);

    return written;
}

/* ... other chat functions ... */
```

### bbs_core.c (Portable!)

```c
#include "pal/pal_sync.h"
#include "pal/pal_time.h"
#include "pal/pal_mem.h"
#include "9p/bbs_core.h"

int bbs_init(struct bbs_instance *bbs)
{
    pal_mutex_init(&bbs->lock);
    bbs->room_count = 0;
    bbs->user_count = 0;

    /* Initialize chat */
    chat_init(&bbs->chat);

    return 0;
}

int bbs_post_message(struct bbs_instance *bbs, const char *room_name,
                     const char *username, const char *body,
                     uint32_t reply_to)
{
    pal_mutex_lock(&bbs->lock);

    struct bbs_room *room = bbs_find_room(bbs, room_name);
    if (!room) {
        pal_mutex_unlock(&bbs->lock);
        return -1;
    }

    if (room->message_count >= MAX_MESSAGES_PER_ROOM) {
        pal_mutex_unlock(&bbs->lock);
        return -ENOSPC;
    }

    struct bbs_message *msg = &room->messages[room->message_count];
    msg->id = room->next_id++;
    pal_strncpy(msg->from, username, MAX_USERNAME - 1);
    pal_strncpy(msg->to, room_name, MAX_ROOMNAME - 1);
    msg->timestamp = pal_time_now();
    msg->reply_to = reply_to;

    msg->body = pal_strdup(body);
    msg->body_len = pal_strlen(body);
    msg->deleted = false;

    room->message_count++;

    pal_mutex_unlock(&bbs->lock);
    return msg->id;
}

/* ... other BBS functions ... */
```

## Build System Integration

### CMakeLists.txt (Zephyr)

```cmake
# Core BBS logic (portable)
zephyr_library_sources(
    src/core/bbs_core.c
    src/core/chat_core.c
)

# Zephyr PAL implementation
zephyr_library_sources(
    src/pal/zephyr/pal_sync_zephyr.c
    src/pal/zephyr/pal_time_zephyr.c
    src/pal/zephyr/pal_mem_zephyr.c
)

# Zephyr-specific 9P integration
zephyr_library_sources(
    src/9bbs/9bbs.c          # Uses core/*
)
```

### Makefile (Unix)

```makefile
# Core BBS logic (portable)
CORE_SRCS = src/core/bbs_core.c \
            src/core/chat_core.c

# Unix PAL implementation
PAL_SRCS = src/pal/unix/pal_sync_unix.c \
           src/pal/unix/pal_time_unix.c \
           src/pal/unix/pal_mem_unix.c

# Unix server (uses core/*)
SERVER_SRCS = src/unix/9pbbs_server.c

SRCS = $(CORE_SRCS) $(PAL_SRCS) $(SERVER_SRCS)
OBJS = $(SRCS:.c=.o)

9pbbs: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
```

## Migration Path

### Step 1: Extract Core Logic

Move shared code from `src/9bbs/9bbs.c` to `src/core/bbs_core.c`:

```c
// Before (9bbs.c - Zephyr-specific)
k_mutex_lock(&bbs->lock, K_FOREVER);
// ... BBS logic ...
k_mutex_unlock(&bbs->lock);

// After (bbs_core.c - portable)
pal_mutex_lock(&bbs->lock);
// ... BBS logic ...
pal_mutex_unlock(&bbs->lock);
```

### Step 2: Create PAL Implementations

Implement each PAL function for both platforms.

### Step 3: Update Build Systems

Add core/ and pal/ to both Zephyr and Unix builds.

### Step 4: Test Both Platforms

Ensure identical behavior between Zephyr and Unix servers.

## Benefits

1. **Single Source of Truth**: BBS logic lives in one place
2. **Bug Fixes Propagate**: Fix once, works everywhere
3. **Feature Parity**: New features automatically available on both
4. **Testable**: Test core logic on Unix (faster iteration)
5. **Portable**: Easy to add new platforms (FreeRTOS, bare metal, etc.)

## Size Impact

**Zephyr:**
- Core logic: ~1200 bytes (same as before)
- PAL overhead: ~200 bytes (thin wrappers)
- **Total: ~1400 bytes** (minimal increase)

**Unix:**
- Core logic: Same source
- PAL overhead: ~500 bytes (pthread wrappers)

## Example: Adding a New Feature

```c
// Add to src/core/chat_core.c (once!)
int chat_set_topic(struct chat_instance *chat, const char *room,
                   const char *topic)
{
    pal_mutex_lock(&chat->lock);
    struct chat_room *r = chat_find_room(chat, room);
    if (r) {
        pal_strncpy(r->topic, topic, sizeof(r->topic) - 1);
    }
    pal_mutex_unlock(&chat->lock);
    return r ? 0 : -1;
}
```

**Works on both Zephyr and Unix immediately!**

## Limitations

**Not abstracted:**
- 9P transport (Bluetooth vs TCP)
- Network stack
- File system backend
- Specific hardware

**These remain platform-specific** and live in `src/9bbs/` (Zephyr) and `src/unix/` (Unix).

## Alternative: Header-Only PAL

For even simpler integration:

```c
// pal_config.h
#ifdef ZEPHYR
  #include <zephyr/kernel.h>
  #define pal_mutex_t struct k_mutex
  #define pal_mutex_lock(m) k_mutex_lock(m, K_FOREVER)
  // ...
#else
  #include <pthread.h>
  #define pal_mutex_t pthread_mutex_t
  #define pal_mutex_lock(m) pthread_mutex_lock(m)
  // ...
#endif
```

**Trade-off:** Simpler, but less type-safe and harder to debug.

## Recommendation

Use the **full PAL approach** (separate .c files) for:
- Better type checking
- Easier debugging
- Cleaner separation
- Room for platform-specific optimizations

The small overhead (~200 bytes on Zephyr) is worth it for the maintainability gains.
