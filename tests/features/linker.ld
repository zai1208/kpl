OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kpl_main)
SECTIONS {
    . = 0xFFFFFFFF80000000; /* maps directly to Limine higher-half rules */
    .text :   { *(.text*) }
    .rodata : { *(.rodata*) }
    .data :   { *(.data*) }
    .bss :    { *(.bss*) *(COMMON) }
}
