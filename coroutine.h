#ifndef COROUTINE_H_
#define COROUTINE_H_

#include <stddef.h>

#define COROUTINE_DEFAULT_STACK_SIZE (8*4096)

typedef union Coroutine {
    struct {
        void* stack_ptr;
        void* stack_base;
        void* stack_top;
        void (*destroy)(void*, size_t);
    };
    int next_free;
} Coroutine;

typedef enum CoroutineMode {
    CM_YIELD,
    CM_WAIT_READ,
    CM_WAIT_WRITE,
} CoroutineMode;

int  coroutine_create(void (*f)(void*), void* data, size_t size, void (*destroy)(void*, size_t));
void coroutine_switch(int fd, CoroutineMode mode);
int  coroutine_id();
void coroutine_wake_up(int id);


#define coroutine_yield()        coroutine_switch(0,  CM_YIELD)
#define coroutine_wait_read(fd)  coroutine_switch(fd, CM_WAIT_READ)
#define coroutine_wait_write(fd) coroutine_switch(fd, CM_WAIT_WRITE)

#endif // COROUTINE_H_



#ifdef COROUTINE_IMPLEMENTATION
#undef COROUTINE_IMPLEMENTATION

#include <assert.h>

#include <poll.h>


#define MAX_CONCURRENT_COROUTINES 1024


static struct pollfd  g_polls[MAX_CONCURRENT_COROUTINES]      = { 0 };
static int            g_sleeping[MAX_CONCURRENT_COROUTINES]   = { 0 };
static int            g_active[MAX_CONCURRENT_COROUTINES]     = { 0 };
static Coroutine      g_coroutines[MAX_CONCURRENT_COROUTINES] = { 0 };

/*
g_polls       Ordered parallel to g_sleeping
g_sleeping    Unordered with indices to g_coroutines
g_active      Unordered with indices to g_coroutines
g_coroutines  Ordered in insertion order (with intrusive free-list?)
*/


static int g_sleep_count     = 0;
static int g_active_count    = 1;
static int g_coroutine_count = 1;
static int g_current_active  = 0;
static int g_first_free      = 0;


#if defined(COROUTINE_MMAP)
    #include <sys/mman.h>
    #ifdef __APPLE__
        #define MAP_STACK 0
        #define MAP_GROWSDOWN 0
    #endif

    void* coroutine_stack_allocate(size_t size) {
        void* ptr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_STACK|MAP_GROWSDOWN, -1, 0);
        return ptr == MAP_FAILED ? NULL : ptr;
    }

    void coroutine_stack_deallocate(void* ptr, size_t size) {
        munmap(ptr, size);
    }
#elif defined(COROUTINE_MALLOC)
    #include <stdlib.h>

    #define oroutine_stack_allocate malloc
    void coroutine_stack_deallocate(void* ptr, size_t size) {
        free(ptr)
    }
#elif defined(coroutine_stack_allocate) && defined(coroutine_stack_deallocate)
#else
    #error "Must define an allocator/deallocator"
#endif


static int safety_check() {
    printf("----------------\n");

    for (int i = 0; i < g_active_count - 1; i++) {
        for (int j = i + 1; j < g_active_count; j++) {
            if (g_active[i] == g_active[j]) {
                printf("%d (%d) and %d (%d) in active!\n", i, g_active[i], j, g_active[j]);
                return 0;
            }
        }
    }

    for (int i = 0; i < g_active_count; i++)
        printf("%03d is active\n", g_active[i]);


    for (int i = 0; i < g_sleep_count - 1; i++) {
        for (int j = i + 1; j < g_sleep_count; j++) {
            if (g_sleeping[i] == g_sleeping[j]) {
                printf("%d (%d) and %d (%d) in sleeping!\n", i, g_active[i], j, g_active[j]);
                return 0;
            }
        }
    }

    for (int i = 0; i < g_sleep_count; i++)
        printf("%03d is sleeping\n", g_sleeping[i]);

    for (int i = 1; i < g_coroutine_count - 1; i++) {
        Coroutine* coroutine = &g_coroutines[i];
        if (coroutine->stack_base == NULL) {
            printf("Null base at %d which is not main\n", i);
            return 0;
        }
    }

    return 1;
}


void coroutine__return_from_current_coroutine();
int coroutine_create(void (*f)(void*), void* data, size_t size, void (*destroy)(void*, size_t))
{
    assert(safety_check());

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

        *(--ptr) = coroutine__return_from_current_coroutine;
        *(--ptr) = f;
        *(--ptr) = ptr_top;                 // push rdi
        *(--ptr) = 0;                       // push rbx
        *(--ptr) = 0;                       // push rbp
        *(--ptr) = 0;                       // push r12
        *(--ptr) = 0;                       // push r13
        *(--ptr) = 0;                       // push r14
        *(--ptr) = 0;                       // push r15

        free->stack_ptr = ptr;

        assert(safety_check());
        return free_index;
    } else if (g_coroutine_count >= MAX_CONCURRENT_COROUTINES) {
        return -1;
    }

    size_t stack_size = COROUTINE_DEFAULT_STACK_SIZE;
    void*  stack = coroutine_stack_allocate(stack_size);
    // TODO: Assert is 16 byte aligned.

    char* stack_top = (char*)stack + stack_size;
    char* ptr_top   = stack_top - size;
    memcpy(ptr_top, data, size);

    void** ptr = (void*) ptr_top;

    *(--ptr) = coroutine__return_from_current_coroutine;
    *(--ptr) = f;
    *(--ptr) = ptr_top;                 // push rdi
    *(--ptr) = 0;                       // push rbx
    *(--ptr) = 0;                       // push rbp
    *(--ptr) = 0;                       // push r12
    *(--ptr) = 0;                       // push r13
    *(--ptr) = 0;                       // push r14
    *(--ptr) = 0;                       // push r15

    Coroutine coroutine = {
        .stack_base = stack,
        .stack_ptr = ptr,
        .stack_top = stack_top,
        .destroy = destroy,
    };

    g_active[g_active_count++] = g_coroutine_count;
    g_coroutines[g_coroutine_count++] = coroutine;

    assert(safety_check());
    return g_coroutine_count-1;
}


// Linux x86_64 call convention
// rdi, rsi, rdx, rcx, r8, and r9 are arguments
// r12, r13, r14, r15, rbx, rsp, rbp are the callee-saved registers
void __attribute__((naked)) coroutine_switch(__attribute__((unused)) int fd, __attribute__((unused)) CoroutineMode mode)
{
    // @arch - Push the `arg` on the stack and then all callee-saved registers. Then jump to `coroutine__switch_context`.
    asm(
    "    pushq %rdi\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, %rdx\n"     // rsp
    "    jmp coroutine__switch_context\n"
    );
}

void __attribute__((naked)) coroutine__restore_context(__attribute__((unused)) void *rsp)
{
    // @arch - Set the stack to `rsp`, restore all callee-saved registers and set the parameter in `rdi`.
    asm(
    "    movq %rdi, %rsp\n"     // Set stack pointer to the new stack
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    popq %rdi\n"
    "    ret\n"
    );
}


void coroutine__poll() {
    assert(safety_check());
    if (g_sleep_count == 0) return;

    int timeout = g_active_count == 0 ? -1 : 0;
    while (poll(g_polls, g_sleep_count, timeout) < 0) {}

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
    assert(safety_check());
}

extern void coroutine__switch_context(int fd, CoroutineMode mode, void *rsp) asm("coroutine__switch_context");
void coroutine__switch_context(int fd, CoroutineMode mode, void *rsp)
{
    assert(safety_check());

    // Set current context rsp
    int active_id = g_active[g_current_active];
    Coroutine* coroutine = &g_coroutines[active_id];
    coroutine->stack_ptr = rsp;

    assert(coroutine->stack_base == NULL || coroutine->stack_base <= coroutine->stack_ptr && coroutine->stack_ptr <= coroutine->stack_top);

    switch (mode) {
        case CM_YIELD: {
            // Go to next coroutine
            g_current_active += 1;
            g_current_active %= g_active_count;
        } break;
        case CM_WAIT_READ:
        case CM_WAIT_WRITE: {
            // Put current coroutine to sleep
            struct pollfd pfd = {
                .fd = fd,
                .events = (mode == CM_WAIT_READ) ? POLLRDNORM : POLLWRNORM,
                .revents = 0
            };

            g_sleeping[g_sleep_count] = active_id;
            g_polls[g_sleep_count] = pfd;
            g_sleep_count += 1;

            assert(g_active_count > 0);
            g_active[g_current_active] = g_active[--g_active_count];
        } break;
    }

    coroutine__poll();

    active_id = g_active[g_current_active];
    coroutine = &g_coroutines[active_id];
    coroutine__restore_context(coroutine->stack_ptr);
}

void coroutine__return_from_current_coroutine()
{
    int current_coroutine_id = g_active[g_current_active];
    assert(current_coroutine_id > 0);
    Coroutine* coroutine = &g_coroutines[current_coroutine_id];

    assert(g_active_count > 0);
    g_active[g_current_active] = g_active[--g_active_count];

    char* stack_base = coroutine->stack_base;
    assert(stack_base != NULL);

    coroutine->next_free = g_first_free;
    g_first_free = current_coroutine_id;

    if (g_active_count == 0) {
        coroutine__poll();
    }

    int next_active_id = g_active[g_current_active];
    Coroutine* next_coroutine = &g_coroutines[next_active_id];
    void* rsp = next_coroutine->stack_ptr;

    assert(rsp != NULL);
    assert(safety_check());
    coroutine__restore_context(rsp);
}


int coroutine_id() {
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


#endif
