/*
 * This file will be firstly configured by CMake to fill in `init_objects`,
 * then preprocessed by C compiler to fill in some definitions from `image.h`.
 */

#include "image.h"

ENTRY(_start)

SECTIONS
{
    . = TEXT_OFFSET;
    img_start = .;
    init : {
        ${init_objects}
    }

    . = ALIGN(SZ_4K);

    init_end = ABSOLUTE(.);

    .text KERNEL_VADDR + init_end : AT(init_end) {/* We use AT to set LMA different from VMA */
        *(.text*)
    }
    text_end = . - KERNEL_VADDR;

    . = ALIGN(SZ_4K);
    data_start = . - KERNEL_VADDR;
    .data : {
        *(.data*)
    }
    data_end = . - KERNEL_VADDR;

    . = ALIGN(SZ_4K);
    rodata_start = . - KERNEL_VADDR;
    .rodata : {
        *(.rodata*)
    }
    _edata = . - KERNEL_VADDR;

    . = ALIGN(SZ_4K);
    _bss_start = . - KERNEL_VADDR;
    .bss : {
        *(.bss*)
    }
    _bss_end = . - KERNEL_VADDR;

    . = ALIGN(SZ_4K);

    img_end = . - KERNEL_VADDR;

    /DISCARD/ : { *(.comment) *(.gnu*) *(.note*) *(.eh_frame*) }
}
