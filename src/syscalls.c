/*
 * syscalls.c — newlib system-call stubs for the bare-metal build.
 * Only _sbrk is real (the grblHAL core uses malloc); everything else
 * satisfies the linker for code paths we never take.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include <errno.h>
#include <sys/stat.h>

extern char __end__;                    /* end of .bss/heap start (linker) */
extern uint32_t __StackTop;

#define STACK_RESERVE 0x2000            /* keep 8 KB clear for the stack */

void *_sbrk (int incr)
{
    static char *heap_end = &__end__;
    char *limit = (char *)&__StackTop - STACK_RESERVE;

    if (heap_end + incr > limit) {
        errno = ENOMEM;
        return (void *)-1;
    }

    char *prev = heap_end;
    heap_end += incr;

    return prev;
}

int _close (int fd) { (void)fd; return -1; }
int _fstat (int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _getpid (void) { return 1; }
int _isatty (int fd) { (void)fd; return 1; }
int _kill (int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _lseek (int fd, int ptr, int dir) { (void)fd; (void)ptr; (void)dir; return 0; }
int _read (int fd, char *ptr, int len) { (void)fd; (void)ptr; (void)len; return 0; }
int _write (int fd, char *ptr, int len) { (void)fd; (void)ptr; return len; }

void _exit (int status)
{
    (void)status;
    while (1) { }
}
