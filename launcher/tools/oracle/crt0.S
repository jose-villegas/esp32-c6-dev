/*=============================================================================
 * crt0 - the entire bare-metal reset path for the oracle image.
 *
 * QEMU's riscv "virt" machine, given "-bios none -kernel <elf>", jumps
 * straight to the ELF's entry point in machine mode - no boot ROM, no
 * OpenSBI, no reset vector to satisfy. So this has exactly three jobs: give
 * the C code a stack, zero .bss (the C standard promises zero-initialised
 * statics and nothing else here will do it), and call main(). Nothing here
 * runs constructors (.init_array) - suite_sand.c's own SUITE_REGISTER
 * constructors would rely on that, but oracle_main.c never calls
 * suites_run_all()/the registry at all, so skipping it is a no-op, not a
 * simplification with a hidden cost. See virt.ld for where _bss_start,
 * _bss_end and _stack_top come from.
 *===========================================================================*/
.section .text.start
.global _start
_start:
    la sp, _stack_top

    la t0, _bss_start
    la t1, _bss_end
1:
    bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:
    call main

    /* main() returns Unity's failure count. A TCG plugin totals its counters
     * at process exit, and this target has no supervisor to exit to - so
     * "done" is just "stop retiring instructions", not a real return. */
3:
    j 3b
