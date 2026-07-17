; boot32.asm - Multiboot2 entry point and long-mode transition.
;
; This file is hand-written NASM, not KPL, and that's not a stopgap - it's
; structural. Every PROC prologue kpl emits is `push rbp` / `mov rbp, rsp`
; using 64-bit register names, which only assembles/executes correctly
; once the CPU is already in long mode. Something has to do the 32-bit ->
; long-mode transition (GDT, paging, EFER.LME, CR0.PG) before the first
; KPL-generated instruction can run at all. That something can never be
; KPL itself - any KPL kernel needs a stub like this one.
;
; Loaded directly by QEMU's `-kernel` (or GRUB) via the Multiboot2 header
; below. Identity-maps the first 1GB with 2MB pages, which is enough to
; reach both this kernel's own load address (1MB) and the VGA text buffer
; (0xB8000) without needing a higher-half layout for this smoke test.

BITS 32

section .multiboot
align 8
mb2_header_start:
    dd 0xE85250D6                                          ; magic
    dd 0                                                    ; architecture: i386 (protected mode)
    dd mb2_header_end - mb2_header_start                    ; header length
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start)) & 0xFFFFFFFF ; checksum
    align 8
    dw 0                                                    ; end tag: type
    dw 0                                                    ; end tag: flags
    dd 8                                                    ; end tag: size
mb2_header_end:

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

align 4096
pml4:   resb 4096
pdpt:   resb 4096
pd:     resb 4096

section .text
global _start
extern kpl_main

_start:
    mov esp, stack_top
    cli

    ; --- zero the three page-table pages ---
    mov edi, pml4
    xor eax, eax
    mov ecx, (4096 * 3) / 4
    rep stosd

    ; --- pml4[0] -> pdpt ---
    mov eax, pdpt
    or eax, 0x3                 ; present | writable
    mov [pml4], eax

    ; --- pdpt[0] -> pd ---
    mov eax, pd
    or eax, 0x3
    mov [pdpt], eax

    ; --- pd[0..511] = 512 * 2MB pages, identity-mapping the first 1GB ---
    mov ecx, 0
.fill_pd:
    mov eax, 0x200000
    mul ecx
    or eax, 0x83                ; present | writable | page-size (2MB)
    mov [pd + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jl .fill_pd

    mov eax, pml4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5               ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080          ; EFER MSR
    rdmsr
    or eax, 1 << 8                ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31               ; CR0.PG
    mov cr0, eax

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

section .rodata
gdt64:
    dq 0
.code: equ $ - gdt64
    dd 0xFFFF
    db 0
    db 10011010b                 ; present, ring0, code, executable, readable
    db 10101111b                 ; 4K granularity, long-mode (L=1), limit high
    db 0
.data: equ $ - gdt64
    dd 0xFFFF
    db 0
    db 10010010b                 ; present, ring0, data, writable
    db 0
    db 0
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .text
BITS 64
long_mode_start:
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top
    call kpl_main                ; hands off to KPL-compiled code

.halt:
    cli
    hlt
    jmp .halt
