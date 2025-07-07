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
// ... include your libraries ...

#define COROUTINE_STACK_MMAP
#define COROUTINE_MAX_COUNT 12      // Define max amount of concurrent coroutines
#define COROUTINE_IMPLEMENTATION    // Include the implementation
#include "coroutine.h"


void echo(void* arg) {
    int fd = (int)(size_t)arg;
    char buffer[128];

    // Wait for the file descriptor to be ready to read.
    coroutine_wait_read(fd);
    
    // This should always be ready.
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n <= 0) break;

    coroutine_wait_write(fd);
    write(fd, buffer, n);

    close(fd);
}


int main(void) {
    // Create a non-blocking file descriptor to a socket or file.
    int fd = ...;
    
    // Dispatch a coroutine (won't start it)
    coroutine_create(echo, (void*)fd, sizeof(fd), NULL);

    while (coroutine_active() > 1) {
        printf("Yielding...\n");
        coroutine_yield();  // Start the coroutine
    }
    
    printf("Done!");

    // Deallocate all coroutines and their stacks.
    coroutine_destroy_all();
    return 0;
}
```

### 3. Example: TCP server

```bash
make test
cd build && ./test
python3 test.py  # In another terminal
```

---

## API Reference

```c
int coroutine_create(
    void (*f)(void*),               // Entry function
    const void* data,               // Argument data (copied to the beginning of the coroutine stack)
    size_t size,                    // Size of argument
    void (*destroy)(void*, size_t)  // Optional destructor for stack/argument
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
