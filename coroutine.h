#ifndef COROUTINE_H_
#define COROUTINE_H_

#include <stddef.h>

typedef enum CoroutineMode {
    CM_YIELD,
    CM_WAIT_READ,
    CM_WAIT_WRITE,
} CoroutineMode;

int  coroutine_create(void (*f)(void*), const void* data, size_t size, void (*destroy)(void*, size_t));
void coroutine_switch(int fd, CoroutineMode mode);
int  coroutine_id(void);
int  coroutine_active(void);
void coroutine_wake_up(int id);
void coroutine_destroy_all(void);


#define coroutine_yield()        coroutine_switch(0,  CM_YIELD)
#define coroutine_wait_read(fd)  coroutine_switch(fd, CM_WAIT_READ)
#define coroutine_wait_write(fd) coroutine_switch(fd, CM_WAIT_WRITE)

#endif // COROUTINE_H_



#ifdef COROUTINE_IMPLEMENTATION
#undef COROUTINE_IMPLEMENTATION

#if !defined(COROUTINE_ASSERT)
#include <assert.h>
#define COROUTINE_ASSERT(x) assert(x)
#endif


#include <poll.h>
#include <signal.h>

#if !defined(COROUTINE_MAX_COUNT)
#define COROUTINE_MAX_COUNT 1024
#endif

#if !defined(COROUTINE_IS_THREADED)
#define COROUTINE_IS_THREADED 8
#endif

#if !COROUTINE_IS_THREADED
#define THREAD_LOCAL static
#else
#define THREAD_LOCAL _Thread_local
#endif


#if !defined(COROUTINE_STACK_SIZE)
#define COROUTINE_STACK_SIZE (8*4096)
#endif


typedef union Coroutine {
    struct {
        void* stack_ptr;
        void* stack_base;
        void* stack_top;
        void (*destroy)(void*, size_t);
    };
    int next_free;
} Coroutine;


/*
g_polls       Ordered parallel to g_sleeping
g_sleeping    Unordered with indices to g_coroutines
g_active      Unordered with indices to g_coroutines
g_coroutines  Ordered in insertion order (with intrusive free-list?)
*/
THREAD_LOCAL struct pollfd  g_polls[COROUTINE_MAX_COUNT]      = { 0 };
THREAD_LOCAL int            g_sleeping[COROUTINE_MAX_COUNT]   = { 0 };
THREAD_LOCAL int            g_active[COROUTINE_MAX_COUNT]     = { 0 };
THREAD_LOCAL Coroutine      g_coroutines[COROUTINE_MAX_COUNT] = { 0 };

THREAD_LOCAL int g_sleep_count     = 0;
THREAD_LOCAL int g_active_count    = 1;
THREAD_LOCAL int g_coroutine_count = 1;
THREAD_LOCAL int g_current_active  = 0;
THREAD_LOCAL int g_first_free      = 0;


#if defined(COROUTINE_STACK_MMAP)
    #include <sys/mman.h>
    #ifdef __APPLE__
        #define MAP_STACK 0
        #define MAP_GROWSDOWN 0
    #endif

    static void* coroutine_stack_allocate(size_t size) {
        void* ptr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_STACK|MAP_GROWSDOWN, -1, 0);
        return ptr == MAP_FAILED ? NULL : ptr;
    }

    static void coroutine_stack_deallocate(void* ptr, size_t size) {
        munmap(ptr, size);
    }
#elif defined(COROUTINE_STACK_MALLOC)
    #include <stdlib.h>

    #define coroutine_stack_allocate malloc
    void coroutine_stack_deallocate(void* ptr, size_t size) {
        free(ptr);
    }
#elif defined(coroutine_stack_allocate) && defined(coroutine_stack_deallocate)
#else
    #error "Must define an allocator/deallocator"
#endif


#if !defined(COROUTINE_LOG)
#define COROUTINE_LOG(id, message, ...)
#endif


static int safety_check(void) {
    for (int i = 0; i < g_active_count - 1; i++) {
        for (int j = i + 1; j < g_active_count; j++) {
            COROUTINE_ASSERT(g_active[i] != g_active[j]);
        }
    }

    for (int i = 0; i < g_sleep_count - 1; i++) {
        for (int j = i + 1; j < g_sleep_count; j++) {
            COROUTINE_ASSERT(g_sleeping[i] != g_sleeping[j]);
        }
    }

    COROUTINE_ASSERT(g_coroutines[0].stack_base == NULL);
    for (int i = 1; i < g_coroutine_count - 1; i++) {
        Coroutine* coroutine = &g_coroutines[i];
        COROUTINE_ASSERT(coroutine->stack_base != NULL);
    }

    return 1;
}


static void coroutine__return_from_current_coroutine(void);
int coroutine_create(void (*f)(void*), const void* data, size_t size, void (*destroy)(void*, size_t))
{
    COROUTINE_ASSERT(safety_check());

    // NOTE: Rounding up size to a multiple of 16 as the stack is required to
    //       be 16-byte aligned on certain architectures.
    size = (size + 15) & ~(size_t)15;

    if (g_first_free != 0) {
        int free_index = g_first_free;
        Coroutine* free = &g_coroutines[free_index];
        g_active[g_active_count++] = free_index;
        g_first_free = free->next_free;
        free->destroy = destroy;

        char* stack_top = (char*)free->stack_top;
        char* ptr_top   = (char*)stack_top - size;
        memcpy(ptr_top, data, size);

        void** ptr = (void*) ptr_top;

        #if defined(__x86_64__)
                *(--ptr) = (void*) coroutine__return_from_current_coroutine;
                *(--ptr) = (void*) f;
                *(--ptr) = (void*) ptr_top;         // push rdi
                *(--ptr) = 0;                       // push rbx
                *(--ptr) = 0;                       // push rbp
                *(--ptr) = 0;                       // push r12
                *(--ptr) = 0;                       // push r13
                *(--ptr) = 0;                       // push r14
                *(--ptr) = 0;                       // push r15
        #elif defined(__aarch64__)
                *(--ptr) = (void*) ptr_top;
                *(--ptr) = (void*) coroutine__return_from_current_coroutine;
                *(--ptr) = (void*) f; // push x30
                *(--ptr) = 0;   // push x29
                *(--ptr) = 0;   // push x28
                *(--ptr) = 0;   // push x27
                *(--ptr) = 0;   // push x26
                *(--ptr) = 0;   // push x25
                *(--ptr) = 0;   // push x24
                *(--ptr) = 0;   // push x23
                *(--ptr) = 0;   // push x22
                *(--ptr) = 0;   // push x21
                *(--ptr) = 0;   // push x20
                *(--ptr) = 0;   // push x19
                *(--ptr) = 0;   // push v15
                *(--ptr) = 0;
                *(--ptr) = 0;   // push v14
                *(--ptr) = 0;
                *(--ptr) = 0;   // push v13
                *(--ptr) = 0;
                *(--ptr) = 0;   // push v12
                *(--ptr) = 0;
                *(--ptr) = 0;   // push v11
                *(--ptr) = 0;
                *(--ptr) = 0;   // push v10
                *(--ptr) = 0;
                *(--ptr) = 0;   // push v09
                *(--ptr) = 0;
                *(--ptr) = 0;   // push v08
                *(--ptr) = 0;
        #else
        #error "Unsupported platform"
        #endif

        free->stack_ptr = ptr;

        COROUTINE_ASSERT(safety_check());
        return free_index;
    } else if (g_coroutine_count >= COROUTINE_MAX_COUNT) {
        return -1;
    }

    size_t stack_size = COROUTINE_STACK_SIZE;
    void*  stack = coroutine_stack_allocate(stack_size);
    // TODO: Assert is 16 byte aligned.

    char* stack_top = (char*)stack + stack_size;
    char* ptr_top   = stack_top - size;
    memcpy(ptr_top, data, size);

    void** ptr = (void*) ptr_top;

    #if defined(__x86_64__)
        *(--ptr) = (void*) coroutine__return_from_current_coroutine;
        *(--ptr) = (void*) f;
        *(--ptr) = (void*) ptr_top;         // push rdi
        *(--ptr) = 0;                       // push rbx
        *(--ptr) = 0;                       // push rbp
        *(--ptr) = 0;                       // push r12
        *(--ptr) = 0;                       // push r13
        *(--ptr) = 0;                       // push r14
        *(--ptr) = 0;                       // push r15
    #elif defined(__aarch64__)
        *(--ptr) = (void*) ptr_top;
        *(--ptr) = (void*) coroutine__return_from_current_coroutine;
        *(--ptr) = (void*) f; // push x30
        *(--ptr) = 0;   // push x29
        *(--ptr) = 0;   // push x28
        *(--ptr) = 0;   // push x27
        *(--ptr) = 0;   // push x26
        *(--ptr) = 0;   // push x25
        *(--ptr) = 0;   // push x24
        *(--ptr) = 0;   // push x23
        *(--ptr) = 0;   // push x22
        *(--ptr) = 0;   // push x21
        *(--ptr) = 0;   // push x20
        *(--ptr) = 0;   // push x19
        *(--ptr) = 0;   // push v15
        *(--ptr) = 0;
        *(--ptr) = 0;   // push v14
        *(--ptr) = 0;
        *(--ptr) = 0;   // push v13
        *(--ptr) = 0;
        *(--ptr) = 0;   // push v12
        *(--ptr) = 0;
        *(--ptr) = 0;   // push v11
        *(--ptr) = 0;
        *(--ptr) = 0;   // push v10
        *(--ptr) = 0;
        *(--ptr) = 0;   // push v09
        *(--ptr) = 0;
        *(--ptr) = 0;   // push v08
        *(--ptr) = 0;
    #else
    #error "Unsupported platform! Only supports x86_64 or Aarch64."
    #endif


    Coroutine coroutine = {
        .stack_base = stack,
        .stack_ptr = ptr,
        .stack_top = stack_top,
        .destroy = destroy,
    };

    g_active[g_active_count++] = g_coroutine_count;
    g_coroutines[g_coroutine_count++] = coroutine;

    COROUTINE_ASSERT(safety_check());
    return g_coroutine_count-1;
}

void coroutine_destroy_all(void) {
    COROUTINE_ASSERT(safety_check());
    COROUTINE_ASSERT(coroutine_id() == 0);

    for (int i = 1; i < g_coroutine_count; i++) {
        Coroutine* coroutine = &g_coroutines[i];
        COROUTINE_ASSERT(coroutine->stack_base != NULL);
        coroutine_stack_deallocate(coroutine->stack_base, (char*)coroutine->stack_top - (char*)coroutine->stack_base);
        COROUTINE_LOG(0, "Destroying coroutine %d at %p", i, coroutine->stack_base);
    }

    g_sleep_count     = 0;
    g_active_count    = 1;
    g_coroutine_count = 1;
    g_current_active  = 0;
    g_first_free      = 0;
}


#if defined(__x86_64__)
// rdi, rsi, rdx, rcx, r8, and r9 are arguments
// r12, r13, r14, r15, rbx, rsp, rbp are the callee-saved registers
#define STORE_REGISTERS                             \
    "    pushq %rdi\n"                              \
    "    pushq %rbp\n"                              \
    "    pushq %rbx\n"                              \
    "    pushq %r12\n"                              \
    "    pushq %r13\n"                              \
    "    pushq %r14\n"                              \
    "    pushq %r15\n"                              \
    "    movq %rsp, %rdx\n"                         \
    "    jmp coroutine__switch_context\n"
#define RESTORE_REGISTERS                           \
    "    movq %rdi, %rsp\n"                         \
    "    popq %r15\n"                               \
    "    popq %r14\n"                               \
    "    popq %r13\n"                               \
    "    popq %r12\n"                               \
    "    popq %rbx\n"                               \
    "    popq %rbp\n"                               \
    "    popq %rdi\n"                               \
    "    ret\n"
#elif defined(__aarch64__)
// x19 to x28 are callee-saved
// x0 to x7 are arguments/return values
#define STORE_REGISTERS                                                         \
    "sub sp,   sp, #240\n"                                                      \
    "stp q8,   q9, [sp, #0]\n"                                                  \
    "stp q10, q11, [sp, #32]\n"                                                 \
    "stp q12, q13, [sp, #64]\n"                                                 \
    "stp q14, q15, [sp, #96]\n"                                                 \
    "stp x19, x20, [sp, #128]\n"                                                \
    "stp x21, x22, [sp, #144]\n"                                                \
    "stp x23, x24, [sp, #160]\n"                                                \
    "stp x25, x26, [sp, #176]\n"                                                \
    "stp x27, x28, [sp, #192]\n"                                                \
    "stp x29, x30, [sp, #208]\n"                                                \
    "str x30, [sp, #224]\n"                                                     \
    "str x0,  [sp, #232]\n"                                                     \
    "mov x2, sp\n"                                                              \
    "b coroutine__switch_context\n"
#define RESTORE_REGISTERS                                                       \
    "mov sp, x0\n"                                                              \
    "ldp q8,   q9, [sp, #0]\n"                                                  \
    "ldp q10, q11, [sp, #32]\n"                                                 \
    "ldp q12, q13, [sp, #64]\n"                                                 \
    "ldp q14, q15, [sp, #96]\n"                                                 \
    "ldp x19, x20, [sp, #128]\n"                                                \
    "ldp x21, x22, [sp, #144]\n"                                                \
    "ldp x23, x24, [sp, #160]\n"                                                \
    "ldp x25, x26, [sp, #176]\n"                                                \
    "ldp x27, x28, [sp, #192]\n"                                                \
    "ldp x29, x30, [sp, #208]\n"                                                \
    "mov x1, x30\n"                                                             \
    "ldr x30, [sp, #224]\n"                                                     \
    "ldr x0,  [sp, #232]\n"                                                     \
    "add sp, sp, #240\n"                                                        \
    "ret x1\n"
#else
#error "Unsupported platform! Only supports x86_64 or Aarch64."
#endif


inline void __attribute__((naked)) coroutine_switch(__attribute__((unused)) int fd, __attribute__((unused)) CoroutineMode mode)
{
    // @arch - Push the `arg` on the stack and then all callee-saved registers. Then jump to `coroutine__switch_context`.
    __asm__ volatile (STORE_REGISTERS);
}

static void __attribute__((naked)) coroutine__restore_context(__attribute__((unused)) void *rsp)
{
    // @arch - Set the stack to `rsp`, restore all callee-saved registers and set the parameter in `rdi`.
    __asm__ volatile (RESTORE_REGISTERS);
}


static void coroutine__poll(void) {
    COROUTINE_ASSERT(safety_check());
    if (g_sleep_count == 0) {
        COROUTINE_LOG(g_active[g_current_active], "none are sleeping%s", "");
        return;
    }

    int timeout = g_active_count == 0 ? -1 : 0;
    while (poll(g_polls, g_sleep_count, timeout) < 0) {
        // NOTE: We got interrupted but not by a wake-up signal.
        if (errno == EINTR && g_active_count > 0) {
            break;
        } else {
            perror("poll");
            COROUTINE_LOG(g_active[g_current_active], "poll returned errno %d with %d active", errno, g_active_count);
        }
    }

    for (int i = 0; i < g_sleep_count;) {
        int id = g_sleeping[i];
        if (g_polls[i].revents != 0) {
            int last_sleep_id = --g_sleep_count;
            g_polls[i]    = g_polls[last_sleep_id];
            g_sleeping[i] = g_sleeping[last_sleep_id];

            g_active[g_active_count++] = id;
        } else {
            i += 1;
        }
    }
    COROUTINE_ASSERT(safety_check());
}


extern void coroutine__switch_context(int fd, CoroutineMode mode, void *rsp) __asm__("coroutine__switch_context");
void coroutine__switch_context(int fd, CoroutineMode mode, void *rsp)
{
    COROUTINE_ASSERT(safety_check());

    // Set current context rsp
    int active_id = g_active[g_current_active];
    Coroutine* coroutine = &g_coroutines[active_id];
    coroutine->stack_ptr = rsp;

    COROUTINE_ASSERT(coroutine->stack_base == NULL || coroutine->stack_base <= coroutine->stack_ptr && coroutine->stack_ptr <= coroutine->stack_top);

    switch (mode) {
        case CM_YIELD: {
            COROUTINE_LOG(g_active[g_current_active], "yielding to coroutine %02d", g_active[g_current_active]);

            // Go to next coroutine
            g_current_active += 1;
            g_current_active %= g_active_count;
        } break;
        case CM_WAIT_READ:
        case CM_WAIT_WRITE: {
            COROUTINE_LOG(g_active[g_current_active], "is waiting for %s", (mode == CM_WAIT_READ) ? "read" : "write");

            // Put current coroutine to sleep
            struct pollfd pfd = {
                .fd = fd,
                .events = (mode == CM_WAIT_READ) ? POLLRDNORM : POLLWRNORM,
                .revents = 0
            };

            g_sleeping[g_sleep_count] = active_id;
            g_polls[g_sleep_count] = pfd;
            g_sleep_count += 1;

            COROUTINE_ASSERT(g_active_count >= 0);
            if (g_active_count > 0)
                g_active[g_current_active] = g_active[--g_active_count];
        } break;
    }

    coroutine__poll();

    active_id = g_active[g_current_active];
    coroutine = &g_coroutines[active_id];
    coroutine__restore_context(coroutine->stack_ptr);
}


static void coroutine__return_from_current_coroutine(void)
{
    int current_coroutine_id = g_active[g_current_active];
    COROUTINE_ASSERT(current_coroutine_id > 0);
    Coroutine* coroutine = &g_coroutines[current_coroutine_id];

    COROUTINE_ASSERT(g_active_count > 0);
    g_active[g_current_active] = g_active[--g_active_count];

    char* stack_base = coroutine->stack_base;
    COROUTINE_ASSERT(stack_base != NULL);

    coroutine->next_free = g_first_free;
    g_first_free = current_coroutine_id;

    if (g_active_count == 0) {
        coroutine__poll();
    }

    int next_active_id = g_active[g_current_active];
    Coroutine* next_coroutine = &g_coroutines[next_active_id];
    void* rsp = next_coroutine->stack_ptr;

    COROUTINE_ASSERT(rsp != NULL);
    COROUTINE_ASSERT(safety_check());
    coroutine__restore_context(rsp);
}


int coroutine_id(void) {
    return g_active[g_current_active];
}


void coroutine_wake_up(int id) {
    for (int i = 0; i < g_sleep_count; i++) {
        if (g_sleeping[i] == id) {
            g_polls[i]    = g_polls[g_sleep_count-1];
            g_sleeping[i] = g_sleeping[g_sleep_count-1];
            g_sleep_count -= 1;
            g_active[g_active_count++] = id;
            return;
        }
    }
}


int coroutine_active(void) {
    return g_active_count;
}


#endif
