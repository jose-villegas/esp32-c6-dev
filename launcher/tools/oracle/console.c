/*=============================================================================
 * console - the newlib syscall floor a freestanding link demands, plus one
 * byte-banged UART, for the QEMU riscv "virt" machine (bd oracle spike).
 *
 * WHY THESE EXACT SYMBOLS: linking against newlib's libc.a (this project's
 * own cross toolchain, C:/Users/.../riscv32-esp-elf/) pulls in printf/malloc/
 * etc., which call down into _write/_sbrk/_read/... - the same handful of
 * "reduced to zero" hooks newlib has always expected the platform to supply
 * (this is not oracle-specific; it is the standard bare-metal-newlib
 * contract). Nothing above this file needs to know QEMU exists.
 *
 * __getreent(): this toolchain's newlib was built reentrant (see
 * riscv32-esp-elf/include/sys/reent.h - _REENT expands to __getreent(), not
 * a plain global), so errno and the stdio locking macros silently resolve
 * to a linker-synthesised stub that always fails unless this is defined.
 * Single hart, no threads: one global reentrancy struct, already default-
 * initialised by libc itself (_impure_ptr), is the whole story.
 *===========================================================================*/
#include <errno.h>
#include <reent.h>
#include <sys/stat.h>

struct _reent*
__getreent(void) {
    return _impure_ptr;
}

/* 16550 UART at 0x10000000 - QEMU riscv "virt" machine's fixed MMIO layout
 * (hw/riscv/virt.c upstream), not a C6 fact; this file only exists inside
 * the oracle image, never linked into firmware. Polling only: THR write is
 * enough to reach QEMU's backend (stdio by default under -nographic), and a
 * plugin counting instructions has no reason to want UART traffic gated on
 * LSR-busy - it would only add instructions to the count this file exists
 * to keep out of the way of. */
#define UART0_BASE 0x10000000u
#define UART0_THR  (*(volatile unsigned char*)(UART0_BASE + 0))

int
_write(int file, char* ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        UART0_THR = (unsigned char)ptr[i];
    }
    return len;
}

int
_close(int file) {
    (void)file;
    return -1;
}

int
_fstat(int file, struct stat* st) {
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int
_isatty(int file) {
    (void)file;
    return 1;
}

int
_lseek(int file, int ptr, int dir) {
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

int
_read(int file, char* ptr, int len) {
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

void
_exit(int code) {
    (void)code;
    for (;;) {}
}

int
_kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

int
_getpid(void) {
    return 1;
}

/* Bump allocator over a fixed span - _heap_start/_heap_limit come from
 * virt.ld, and _heap_limit is _heap_start + HEAP_SIZE, where HEAP_SIZE is
 * build_oracle.sh's --defsym straight from DP_FREE_HEAP_BYTES. That is the
 * whole point: a fixture whose grid allocation does not fit in the real
 * board's free heap fails here with ENOMEM the same way it fails on
 * device, instead of quietly succeeding against virt's much larger RAM. */
extern char _heap_start;
extern char _heap_limit;
static char* heap_end = &_heap_start;

void*
_sbrk(int incr) {
    char* prev = heap_end;
    if (heap_end + incr > &_heap_limit) {
        errno = ENOMEM;
        return (void*)-1;
    }
    heap_end += incr;
    return prev;
}
