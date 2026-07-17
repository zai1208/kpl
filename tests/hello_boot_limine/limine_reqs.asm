; limine_reqs.asm - Limine boot protocol markers/requests, plus a single
; hand-written framebuffer-pointer accessor.
;
; Unlike boot32.asm (the Multiboot2/hello_boot stub), this file is almost
; entirely *data*, not code. Limine's native protocol hands off directly
; in 64-bit long mode with paging, a GDT, and a stack already set up - so
; there's no 32-bit entry point or mode transition to hand-write at all.
; What's left is the protocol's data side: a set of magic-tagged "request"
; structs that Limine's loader scans for in the mapped kernel image
; *before* jumping to the entry point, filling in a response pointer for
; each one it understands. KPL has no struct-literal syntax and can't
; express this - but conveniently, it also doesn't need to: these bytes
; just need to exist somewhere in the linked image, in the right section,
; for the bootloader to find. Layout matches limine.h exactly (see
; https://github.com/limine-bootloader/limine, v9.x).
;
; get_request_ptr() and write_pixel32() (further down) are the only
; hand-written code left in this file - see their own comments for why
; each is still unavoidable. Everything else, including the pointer
; chase to find the framebuffer (response -> framebuffers -> [0]), is
; real KPL now (see src/main.kpl), using chained field access and
; literal array indexing (spec/LANGUAGE.md 2.2).
;
; kpl_main is the direct ELF entry point this time (see linker.ld) - no
; handoff stub needed, since by the time Limine calls it, we're already in
; exactly the environment a KPL PROC prologue assumes.

BITS 64

section .limine_requests_start
limine_requests_start_marker:
    dq 0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf
    dq 0x785c6ed015d3e316, 0x181e920a7852b9d9

section .limine_requests
align 8
limine_base_revision:
    dq 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 6

align 8
framebuffer_request:
    dq 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b   ; LIMINE_COMMON_MAGIC
    dq 0x9d5827dcd881dd75, 0xa3148604f6fab11b   ; framebuffer-request-specific id
    dq 0                                          ; revision (requested)
    dq 0                                          ; response - Limine fills this in

section .limine_requests_end
limine_requests_end_marker:
    dq 0xadc0e0531bb10d03, 0x9572709f31764c62

; get_request_ptr() is now the *only* hand-written accessor, and it does
; almost nothing: it returns the address of framebuffer_request itself.
; That's still unavoidable - framebuffer_request lives in this file as
; raw data, and KPL has no address-of operator to reach it directly from
; KPL source - but everything past that address is now real KPL: walking
; response -> framebuffers -> [0] is a chained field-access expression
; (see spec/LANGUAGE.md 2.2), not hand-written pointer chasing. Earlier
; versions of this file did that chase by hand because KPL had no
; multi-hop `->` and no literal `[N]` indexing yet.
section .text
global get_request_ptr
get_request_ptr:
    lea rax, [framebuffer_request]
    ret

; write_pixel32(addr: ptr, val: u32): the other unavoidable hand-written
; piece - KPL's `[var]` substitution only reaches a named variable's own
; fixed stack slot, never a pointer *value* held in one, so writing
; through a computed address still needs to be a real routine rather
; than expressible KPL syntax. This, get_request_ptr() above, and serial
; port I/O (no KPL primitive for `in`/`out`) are the entire remaining
; hand-written surface - everything else in the pixel-write path,
; including the pointer chase to find the framebuffer, is real KPL.
global write_pixel32
write_pixel32:                            ; rdi = addr, esi = val
    mov [rdi], esi
    ret
