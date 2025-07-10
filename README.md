# Coroutine Library for C [![CI](https://github.com/Naxaes/coroutines/actions/workflows/test.yml/badge.svg)](https://github.com/Naxaes/coroutines/actions/workflows/test.yml)

A high-performance, portable, stackful coroutine library for C, supporting `x86_64` and `AArch64`. 

---

## Features

* Stackful coroutines
* Can yield or block on read or write events on file descriptors (`poll()`-based scheduling) 
* Manual coroutine stack allocation
* Supports both `x86_64` and `AArch64`
* Header-only library (stb-style)

---

## Getting Started

### 1. Add to Your Project

```c
#define COROUTINE_STACK_MMAP      // Optional allocation strategy
#define COROUTINE_IMPLEMENTATION
#include "coroutine.h"
```

### 2. Example: Echo example

```c
#include <stdio.h>

#define COROUTINE_STACK_SIZE (4096)
#define COROUTINE_STACK_MALLOC
#define COROUTINE_IMPLEMENTATION
#include "coroutine.h"


void my_task(void* arg) {
    int n = *(int*)arg;

    printf("Starting coroutine %d for %d iterations\n", coroutine_id(), n);
    for (int i = 0; i < n; i++) {
        printf("Coroutine %d on iteration %d\n", coroutine_id(), i);
        coroutine_yield();
    }
}


int main() {
    int n;

    n = 20; coroutine_create(my_task, &n, sizeof(n), NULL);
    n = 10; coroutine_create(my_task, &n, sizeof(n), NULL);
    n = 15; coroutine_create(my_task, &n, sizeof(n), NULL);

    while (coroutine_active() > 1) {
        printf("Main is yielding...\n");
        coroutine_yield();  // Start the coroutine
    }

    printf("Done\n");
}
```

### 3. Example: TCP server

```bash
make test
./build/test
python3 test.py  # In another terminal
```

---

## API Reference

```c
int coroutine_create(
    void (*f)(void*),                   // Entry function
    const void* data,                   // Argument data (copied to the beginning of the coroutine stack)
    size_t size,                        // Size of argument
    void (*on_destroy)(void*, size_t)   // Optional destructor for stack/argument
);

int  coroutine_id(void);                           // Current coroutine ID
int  coroutine_active(void);                       // Amount of currently running coroutines
void coroutine_wake_up(int id);                    // Wake a sleeping coroutine
void coroutine_destroy_all(void);                  // Free all coroutine stacks

void coroutine_switch(int fd, CoroutineMode mode); // Internal context switcher

// Convinence macros
#define coroutine_yield()        coroutine_switch(0, CM_YIELD)
#define coroutine_wait_read(fd)  coroutine_switch(fd, CM_WAIT_READ)
#define coroutine_wait_write(fd) coroutine_switch(fd, CM_WAIT_WRITE)
```

---

## Configuration Options

Define these macros **before** including `coroutine.h` to customize behavior:

| Macro                                                   | Description                                        |
|---------------------------------------------------------|----------------------------------------------------|
| `COROUTINE_STACK_MMAP`                                  | Use `mmap` for stack allocation                    |
| `COROUTINE_STACK_MALLOC`                                | Use `malloc` for stack allocation                  |
| `coroutine_stack_allocate`/`coroutine_stack_deallocate` | User-defined function for stack allocation         |
| `COROUTINE_MAX_COUNT`                                   | Max number of coroutines (default: 1024)           |
| `COROUTINE_STACK_SIZE`                                  | Stack size in bytes per coroutine (default: 32 KB) |
| `COROUTINE_IS_THREADED`                                 | Whether to use static or thread-local variables    |
| `COROUTINE_ASSERT(x)`                                   | Customize assert macro (default: `assert(x)`)      |
| `COROUTINE_LOG(id, messsage, ...)`                      | Hook for logging coroutine events                  |

---

## Limitations/considerations

* Only supported for *Linux* or *macOS* for **x86\_64** or **AArch64**.
* It's not as memory efficient as stackless coroutines (and it's stackful as stackless requires language support), and context switches are slower.
* SIMD registers are currently not saved.
* Only working with clang as GCC doesn't support naked functions.
