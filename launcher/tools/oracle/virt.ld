/*=============================================================================
 * Linker script for QEMU's generic riscv32 "virt" machine (DP_QEMU_MACHINE
 * in the device profile) - not the C6 itself, which has no QEMU machine
 * model at all (see this directory's README.md, "Route A is dead").
 *
 * RAM base 0x80000000 and the entry address matching it are the "virt"
 * machine's own fixed layout (hw/riscv/virt.c upstream: DRAM starts at
 * 0x80000000; "-bios none -kernel <elf>" jumps straight to the ELF entry,
 * no boot ROM in the way). LENGTH is generous headroom, not a real budget -
 * unlike DP_FREE_HEAP_BYTES below, virt's RAM size is a QEMU "-m" flag, not
 * a fact about the C6, so nothing here should ever try to make it look like
 * one.
 *
 * The heap is the one region actually sized to mean something: HEAP_SIZE is
 * injected by build_oracle.sh via --defsym, straight from the device
 * profile's DP_FREE_HEAP_BYTES, so a fixture that overallocates fails here
 * the same way it fails on device - not because virt's RAM ran out (it
 * has far more than the C6 ever will), but because the oracle's heap is
 * deliberately capped to the real board's number. The PROVIDE() default
 * only matters if this script is ever linked without that --defsym (it
 * should never be, but a script that hard-fails instead of degrading
 * silently isn't the goal of one link-script default).
 *===========================================================================*/
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        *(.text.start)
        *(.text .text.*)
    } > RAM

    .rodata : {
        *(.rodata .rodata.*)
    } > RAM

    .data : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } > RAM

    .bss : {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        _bss_end = .;
    } > RAM

    . = ALIGN(16);
    _heap_start = .;
    PROVIDE(HEAP_SIZE = 0x10000);
    _heap_limit = _heap_start + HEAP_SIZE;
    . = _heap_limit;

    /* A fixed-size stack past the heap, out of the same "generous, not a
     * fact about the C6" budget as RAM's own LENGTH above - main-task stack
     * fidelity (DP_MAIN_TASK_STACK_BYTES) is check_stack_usage.py's job on
     * the host runner, not this image's. */
    . = ALIGN(16);
    . = . + 0x40000;
    _stack_top = .;
}
