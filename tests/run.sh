#!/bin/sh
# Smoke/regression suite for the KPL toolchain.
#
# Runs each fixture under tests/ through the full pipeline
# (kpp | kpl -o kernel.asm | nasm | ld) and checks a few structural
# properties of the output. Also exercises kpp's cyclic-INCLUDE handling
# and kpl's rejection of invalid programs.
#
# Usage: ./tests/run.sh   (run from the repo root, after `make`)

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KPP="$ROOT/kpp"
KPL="$ROOT/kpl"
FAIL=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=1; }

if [ ! -x "$KPP" ] || [ ! -x "$KPL" ]; then
    echo "error: kpp/kpl not built - run 'make' first" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

build_fixture() {
    name="$1"
    dir="$ROOT/tests/$name"
    out="$WORK/$name"
    mkdir -p "$out"
    if "$KPP" "$dir/src/main.kpl" > "$out/flat.pp" \
        && "$KPL" -o "$out/kernel.asm" < "$out/flat.pp" \
        && nasm -f elf64 "$out/kernel.asm" -o "$out/kernel.o" 2>/dev/null \
        && ld -nostdlib -z max-page-size=0x1000 -T "$dir/linker.ld" "$out/kernel.o" -o "$out/kernel.bin" 2>/dev/null
    then
        pass "$name: full pipeline (kpp | kpl | nasm | ld)"
    else
        fail "$name: full pipeline"
    fi
}

build_fixture hello_kernel
build_fixture features

# Optional: an actual boot test. hello_boot pairs a hand-written 32-bit
# Multiboot2 entry stub (boot32.asm, necessarily outside the KPL pipeline -
# see its header comment for why) with a real kpl_main that writes
# "Hello, World!" to both VGA and COM1. If qemu/grub are available this
# builds a GRUB ISO, boots it for real, and checks the serial capture -
# this is the only check here that proves KPL-generated code actually
# executes correctly on a CPU, not just that it assembles and links.
if command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v grub-mkrescue >/dev/null 2>&1; then
    hb="$ROOT/tests/hello_boot"
    out="$WORK/hello_boot"
    mkdir -p "$out/isodir/boot/grub"
    if "$KPP" "$hb/src/main.kpl" > "$out/flat.pp" \
        && "$KPL" -o "$out/hello.asm" < "$out/flat.pp" \
        && nasm -f elf64 "$hb/boot32.asm" -o "$out/boot32.o" 2>/dev/null \
        && nasm -f elf64 "$out/hello.asm" -o "$out/hello.o" 2>/dev/null \
        && ld -nostdlib -T "$hb/linker.ld" "$out/boot32.o" "$out/hello.o" -o "$out/isodir/boot/kernel.elf"
    then
        cat > "$out/isodir/boot/grub/grub.cfg" <<'EOF'
set timeout=0
menuentry "hello_boot" {
    multiboot2 /boot/kernel.elf
    boot
}
EOF
        if grub-mkrescue -o "$out/kernel.iso" "$out/isodir" >/dev/null 2>&1; then
            timeout -s KILL 8 qemu-system-x86_64 -cdrom "$out/kernel.iso" \
                -serial file:"$out/serial.log" -display none -no-reboot -m 128M \
                < /dev/null > /dev/null 2>&1 || true
            if grep -q "Hello, World!" "$out/serial.log" 2>/dev/null; then
                pass "hello_boot: booted in QEMU, serial output correct"
            else
                fail "hello_boot: booted but expected serial output missing"
            fi
        else
            fail "hello_boot: grub-mkrescue failed"
        fi
    else
        fail "hello_boot: build (nasm/kpp/kpl/ld) failed"
    fi
else
    echo "SKIP: hello_boot (qemu-system-x86_64 and/or grub-mkrescue not installed)"
fi

# Same idea as hello_boot above, but on the real intended bootloader
# (Limine, native protocol) at the real higher-half load address the
# other fixtures assume, rather than GRUB+Multiboot2 at a flat 1MB. See
# hello_boot_limine/limine_reqs.asm for why the hand-written companion
# file shrinks to almost pure data once Limine (not a hand-rolled 32-bit
# stub) is doing the mode transition. The Limine binary release is
# fetched fresh (not vendored) and pinned to the exact commit this was
# tested against, to keep the repo free of binary blobs without letting
# the test silently drift if upstream changes something.
LIMINE_PIN="ee5d29cd0a8034612dcd1df3f00052480db785c5"
if command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v xorriso >/dev/null 2>&1 && command -v git >/dev/null 2>&1; then
    hbl="$ROOT/tests/hello_boot_limine"
    out="$WORK/hello_boot_limine"
    mkdir -p "$out/iso_root/boot/limine" "$out/iso_root/EFI/BOOT"

    if git clone -q --filter=blob:none https://github.com/limine-bootloader/limine.git "$out/limine-src" 2>/dev/null \
        && git -C "$out/limine-src" -c advice.detachedHead=false checkout -q "$LIMINE_PIN" 2>/dev/null \
        && cc -std=c99 -O2 -o "$out/limine-deploy" "$out/limine-src/limine.c" 2>/dev/null
    then
        if "$KPP" "$hbl/src/main.kpl" > "$out/flat.pp" \
            && "$KPL" -o "$out/hello.asm" < "$out/flat.pp" \
            && nasm -f elf64 "$hbl/limine_reqs.asm" -o "$out/limine_reqs.o" 2>/dev/null \
            && nasm -f elf64 "$out/hello.asm" -o "$out/hello.o" 2>/dev/null \
            && ld -nostdlib -T "$hbl/linker.ld" "$out/limine_reqs.o" "$out/hello.o" -o "$out/iso_root/boot/kernel.elf"
        then
            cp "$hbl/limine.conf" "$out/iso_root/boot/limine/limine.conf"
            cp "$out/limine-src/limine-bios.sys" "$out/limine-src/limine-bios-cd.bin" "$out/limine-src/limine-uefi-cd.bin" "$out/iso_root/boot/limine/" 2>/dev/null
            cp "$out/limine-src/BOOTX64.EFI" "$out/limine-src/BOOTIA32.EFI" "$out/iso_root/EFI/BOOT/" 2>/dev/null

            if xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
                    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
                    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
                    -efi-boot-part --efi-boot-image --protective-msdos-label \
                    "$out/iso_root" -o "$out/kernel.iso" >/dev/null 2>&1 \
                && "$out/limine-deploy" bios-install "$out/kernel.iso" >/dev/null 2>&1
            then
                timeout -s KILL 8 qemu-system-x86_64 -cdrom "$out/kernel.iso" \
                    -serial file:"$out/serial.log" -display none -no-reboot -m 256M \
                    < /dev/null > /dev/null 2>&1 || true
                if grep -q "Hello, World!" "$out/serial.log" 2>/dev/null; then
                    pass "hello_boot_limine: booted via real Limine, serial output correct"
                else
                    fail "hello_boot_limine: booted but expected serial output missing"
                fi
            else
                fail "hello_boot_limine: ISO build/install failed"
            fi
        else
            fail "hello_boot_limine: build (nasm/kpp/kpl/ld) failed"
        fi
    else
        echo "SKIP: hello_boot_limine (could not fetch/build limine)"
    fi
else
    echo "SKIP: hello_boot_limine (qemu-system-x86_64, xorriso, and/or git not installed)"
fi

# tree-shaking: unused_dead_code must not survive into the final asm
if ! grep -q unused_dead_code "$WORK/features/kernel.asm"; then
    pass "features: dead code eliminated by kpp"
else
    fail "features: unused_dead_code leaked into output"
fi

# STRUCT must never emit a label/global for itself
if ! grep -q "global Framebuffer" "$WORK/features/kernel.asm"; then
    pass "features: STRUCT emits no assembly"
else
    fail "features: STRUCT unexpectedly emitted assembly"
fi

# nested if/else and loop label generation sanity check
if grep -q ".L_LOOP_" "$WORK/features/kernel.asm" && grep -q ".L_ELSE_" "$WORK/features/kernel.asm"; then
    pass "features: if/else and loop labels generated"
else
    fail "features: expected control-flow labels missing"
fi

# cyclic INCLUDE must not hang/crash kpp
mkdir -p "$WORK/cycle"
cat > "$WORK/cycle/a.kpl" <<'EOF'
INCLUDE "b.kpl"
PROC kpl_main() { ASM { hlt } }
EOF
cat > "$WORK/cycle/b.kpl" <<'EOF'
INCLUDE "a.kpl"
PROC helper() { ASM { nop } }
EOF
if "$KPP" "$WORK/cycle/a.kpl" > "$WORK/cycle/out.pp"; then
    pass "kpp: cyclic INCLUDE terminates cleanly"
else
    fail "kpp: cyclic INCLUDE failed"
fi

# linearity violation must be rejected
mkdir -p "$WORK/bad"
cat > "$WORK/bad/linearity.kpl" <<'EOF'
PROC kpl_main() {
    u64 a = 1
    u64 b = 2
    u64 c = 3
    u64 crash = a + b * c
}
EOF
if "$KPP" "$WORK/bad/linearity.kpl" | "$KPL" -o "$WORK/bad/out.asm" 2>/dev/null; then
    fail "kpl: linearity violation was NOT rejected"
else
    pass "kpl: linearity violation correctly rejected"
fi

# >6 PROC parameters must be rejected
cat > "$WORK/bad/params.kpl" <<'EOF'
PROC toomany(a: u64, b: u64, c: u64, d: u64, e: u64, f: u64, g: u64) {
    ASM { nop }
}
EOF
if "$KPL" -o "$WORK/bad/out2.asm" < "$WORK/bad/params.kpl" 2>/dev/null; then
    fail "kpl: >6 parameters was NOT rejected"
else
    pass "kpl: >6 parameters correctly rejected"
fi

# -I include roots: two sibling files at the same depth cross-including
# each other via root-relative paths (no "../"), only resolvable with -I
mkdir -p "$WORK/iroot/kstd/arch/x86_64" "$WORK/iroot/kstd/mem"
cat > "$WORK/iroot/kstd/arch/x86_64/io.kpl" <<'EOF'
PROC port_out8(port: u64, val: u64) { ASM { nop } }
EOF
cat > "$WORK/iroot/kstd/mem/pmm.kpl" <<'EOF'
INCLUDE "kstd/arch/x86_64/io.kpl"
PROC alloc_frame() { port_out8(1, 2) }
EOF
cat > "$WORK/iroot/main.kpl" <<'EOF'
INCLUDE "kstd/mem/pmm.kpl"
PROC kpl_main() { alloc_frame() }
EOF
if (cd "$WORK/iroot" && "$KPP" -I. main.kpl > /dev/null); then
    pass "kpp: -I resolves project-root-relative INCLUDE paths"
else
    fail "kpp: -I root-relative resolution failed"
fi
if (cd "$WORK/iroot" && "$KPP" main.kpl > /dev/null 2>&1); then
    fail "kpp: root-relative INCLUDE unexpectedly resolved without -I"
else
    pass "kpp: root-relative INCLUDE correctly fails without -I"
fi

# calling a routine only defined in another translation unit must emit
# `extern` for it, or the nasm assemble step fails - this is what makes
# KPL code able to call hand-written kstd primitives (e.g. hello_boot's
# own boot32.asm calling back into kpl_main, or vice versa)
mkdir -p "$WORK/externcall"
cat > "$WORK/externcall/t.kpl" <<'EOF'
PROC kpl_main() {
    external_routine(1, 2)
}
EOF
cat > "$WORK/externcall/other.asm" <<'EOF'
section .text
global external_routine
external_routine:
    ret
EOF
if "$KPP" "$WORK/externcall/t.kpl" | "$KPL" -o "$WORK/externcall/out.asm" \
    && grep -q "^extern external_routine" "$WORK/externcall/out.asm" \
    && nasm -f elf64 "$WORK/externcall/out.asm" -o "$WORK/externcall/out.o" 2>/dev/null \
    && nasm -f elf64 "$WORK/externcall/other.asm" -o "$WORK/externcall/other.o" 2>/dev/null \
    && ld -nostdlib "$WORK/externcall/out.o" "$WORK/externcall/other.o" -o "$WORK/externcall/linked" 2>/dev/null
then
    pass "kpl: auto-extern lets KPL call routines in another object file"
else
    fail "kpl: auto-extern for cross-file calls failed"
fi

# STRUCT field access (var->field): read, write, and multi-field offset
# correctness, independent of the full boot pipeline above.
mkdir -p "$WORK/structfield"
cat > "$WORK/structfield/t.kpl" <<'EOF'
STRUCT Point {
    u32 tag
    ptr next
    u64 x
    u64 y
}

PROC kpl_main() {
    Point p = 0
    ASM {
        mov rax, 0x2000
        mov [p], rax
    }
    u64 a = p->x
    u64 b = p->y
    p->x = 42
    u64 c = a + b
}
EOF
if "$KPP" "$WORK/structfield/t.kpl" | "$KPL" -o "$WORK/structfield/out.asm" 2>/dev/null \
    && grep -q "\[r11+16\]" "$WORK/structfield/out.asm"  \
    && grep -q "\[r11+24\]" "$WORK/structfield/out.asm"  \
    && nasm -f elf64 "$WORK/structfield/out.asm" -o "$WORK/structfield/out.o" 2>/dev/null
then
    pass "kpl: STRUCT field read/write with correct multi-field offsets"
else
    fail "kpl: STRUCT field access offsets wrong or failed to assemble"
fi

# unknown field on a real struct must be rejected
cat > "$WORK/structfield/bad.kpl" <<'EOF'
STRUCT Point {
    u64 x
    u64 y
}
PROC kpl_main() {
    Point p = 0
    u64 z = p->not_a_field
}
EOF
if "$KPP" "$WORK/structfield/bad.kpl" | "$KPL" -o "$WORK/structfield/bad.asm" 2>/dev/null; then
    fail "kpl: unknown struct field was NOT rejected"
else
    pass "kpl: unknown struct field correctly rejected"
fi

# PROC return values: RETURN <expr>, and capturing a call's result
# directly into a declaration (u64 a = f(x)).
mkdir -p "$WORK/retval"
cat > "$WORK/retval/t.kpl" <<'EOF'
PROC add_five(x: u64) {
    RETURN x + 5
}
PROC get_const() {
    RETURN 42
}
PROC kpl_main() {
    u64 a = add_five(10)
    u64 b = get_const()
    u64 c = a + b
}
EOF
if "$KPP" "$WORK/retval/t.kpl" | "$KPL" -o "$WORK/retval/out.asm" 2>/dev/null \
    && grep -q "call add_five" "$WORK/retval/out.asm" \
    && grep -q "call get_const" "$WORK/retval/out.asm" \
    && nasm -f elf64 "$WORK/retval/out.asm" -o "$WORK/retval/out.o" 2>/dev/null
then
    pass "kpl: RETURN <expr> and capturing a call's result both work"
else
    fail "kpl: PROC return values failed"
fi

# a call expression combined with an operator on the same line is still
# a linearity violation - it's one flat operand, not composable
cat > "$WORK/retval/bad.kpl" <<'EOF'
PROC get_const() {
    RETURN 42
}
PROC kpl_main() {
    u64 a = get_const() + 1
}
EOF
if "$KPP" "$WORK/retval/bad.kpl" | "$KPL" -o "$WORK/retval/bad.asm" 2>/dev/null; then
    fail "kpl: call-expression combined with an operator was NOT rejected"
else
    pass "kpl: call-expression combined with an operator correctly rejected"
fi

# Multi-hop field chains (a->b->c) and literal array indexing (a[N]),
# including the exact real-world shape this was built for:
# req->response->framebuffers[0]->field
mkdir -p "$WORK/chain"
cat > "$WORK/chain/t.kpl" <<'EOF'
STRUCT Framebuffer {
    ptr address
    u64 width
}
STRUCT LimineResponse {
    u64 revision
    u64 framebuffer_count
    ptr framebuffers
}
STRUCT FramebufferRequest {
    u64 revision
    LimineResponse response
}
PROC kpl_main() {
    FramebufferRequest req = 0
    ASM {
        mov rax, 0x1234
        mov [req], rax
    }
    Framebuffer fb = req->response->framebuffers[0]
    u64 w = fb->width
}
EOF
if "$KPP" "$WORK/chain/t.kpl" | "$KPL" -o "$WORK/chain/out.asm" 2>/dev/null \
    && grep -q "mov r11, \[r11+16\]" "$WORK/chain/out.asm" \
    && grep -q "mov rax, \[r11+0\]" "$WORK/chain/out.asm" \
    && nasm -f elf64 "$WORK/chain/out.asm" -o "$WORK/chain/out.o" 2>/dev/null
then
    pass "kpl: multi-hop chain + literal array index (req->response->framebuffers[0])"
else
    fail "kpl: multi-hop chain / array index failed"
fi

# chaining '->' past a '[...]' index must be rejected (its element type
# isn't tracked, by design)
cat > "$WORK/chain/bad1.kpl" <<'EOF'
STRUCT Framebuffer {
    ptr address
}
STRUCT LimineResponse {
    ptr framebuffers
}
STRUCT Req {
    LimineResponse response
}
PROC kpl_main() {
    Req r = 0
    u64 x = r->response->framebuffers[0]->address
}
EOF
if "$KPP" "$WORK/chain/bad1.kpl" | "$KPL" -o "$WORK/chain/bad1.asm" 2>/dev/null; then
    fail "kpl: chaining '->' past a '[...]' index was NOT rejected"
else
    pass "kpl: chaining '->' past a '[...]' index correctly rejected"
fi

# a variable (non-literal) array index must be rejected
cat > "$WORK/chain/bad2.kpl" <<'EOF'
STRUCT Foo {
    ptr items
}
PROC kpl_main() {
    Foo f = 0
    u64 i = 2
    ptr x = f->items[i]
}
EOF
if "$KPP" "$WORK/chain/bad2.kpl" | "$KPL" -o "$WORK/chain/bad2.asm" 2>/dev/null; then
    fail "kpl: variable array index was NOT rejected"
else
    pass "kpl: variable array index correctly rejected"
fi

# Bitwise/shift/unary operators: verify actual computed *results*, not
# just that the codegen looks plausible - boot a tiny kernel (same
# Multiboot2/GRUB harness as hello_boot above) that prints each result
# as a raw byte over serial and check the numbers are right.
# 12 & 10 = 8 ; 12 | 10 = 14 ; 12 ^ 10 = 6 ; 3 << 4 = 48 ; 200 >> 3 = 25 ; ~0 low byte = 255
if command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v grub-mkrescue >/dev/null 2>&1; then
    bw="$WORK/bitwise"
    mkdir -p "$bw/isodir/boot/grub"
    cat > "$bw/t.kpl" <<'EOF'
PROC serial_init() {
    ASM {
        mov dx, 0x3F9
        mov al, 0x00
        out dx, al
        mov dx, 0x3FB
        mov al, 0x80
        out dx, al
        mov dx, 0x3F8
        mov al, 0x03
        out dx, al
        mov dx, 0x3F9
        mov al, 0x00
        out dx, al
        mov dx, 0x3FB
        mov al, 0x03
        out dx, al
        mov dx, 0x3FA
        mov al, 0xC7
        out dx, al
        mov dx, 0x3FC
        mov al, 0x0B
        out dx, al
    }
}
PROC serial_putc(ch: u64) {
    ASM {
    .wait:
        mov dx, 0x3FD
        in al, dx
        test al, 0x20
        jz .wait
        mov al, [ch]
        mov dx, 0x3F8
        out dx, al
    }
}
PROC kpl_main() {
    serial_init()
    u64 a = 12
    u64 b = 10
    u64 r_and = a & b
    u64 r_or = a | b
    u64 r_xor = a ^ b
    u64 r_shl = 3 << 4
    u64 r_shr = 200 >> 3
    u64 zero = 0
    u64 r_not = ~ zero
    serial_putc(r_and)
    serial_putc(r_or)
    serial_putc(r_xor)
    serial_putc(r_shl)
    serial_putc(r_shr)
    serial_putc(r_not)
    ASM {
        cli
    .halt:
        hlt
        jmp .halt
    }
}
EOF
    if "$KPP" "$bw/t.kpl" | "$KPL" -o "$bw/kernel.asm" 2>/dev/null \
        && nasm -f elf64 "$ROOT/tests/hello_boot/boot32.asm" -o "$bw/boot32.o" 2>/dev/null \
        && nasm -f elf64 "$bw/kernel.asm" -o "$bw/kernel.o" 2>/dev/null \
        && ld -nostdlib -T "$ROOT/tests/hello_boot/linker.ld" "$bw/boot32.o" "$bw/kernel.o" -o "$bw/isodir/boot/kernel.elf" 2>/dev/null
    then
        cat > "$bw/isodir/boot/grub/grub.cfg" <<'EOF'
set timeout=0
menuentry "bitwise" {
    multiboot2 /boot/kernel.elf
    boot
}
EOF
        if grub-mkrescue -o "$bw/kernel.iso" "$bw/isodir" >/dev/null 2>&1; then
            timeout -s KILL 8 qemu-system-x86_64 -cdrom "$bw/kernel.iso" \
                -serial file:"$bw/serial.log" -display none -no-reboot -m 128M \
                < /dev/null > /dev/null 2>&1 || true
            got="$(od -An -tu1 "$bw/serial.log" 2>/dev/null | tr -s ' ')"
            want=" 8 14 6 48 25 255"
            if [ "$got" = "$want" ]; then
                pass "kpl: bitwise/shift/unary operators produce correct results (booted, verified over serial)"
            else
                fail "kpl: bitwise/shift/unary results wrong - got '$got', want '$want'"
            fi
        else
            fail "kpl: bitwise test grub-mkrescue failed"
        fi
    else
        fail "kpl: bitwise test build failed"
    fi
else
    echo "SKIP: bitwise operator boot verification (qemu-system-x86_64 and/or grub-mkrescue not installed)"
fi

if [ "$FAIL" -eq 0 ]; then
    echo "All checks passed."
else
    echo "Some checks FAILED." >&2
fi
exit "$FAIL"
