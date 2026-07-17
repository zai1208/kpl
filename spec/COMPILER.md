# KPL Compiler spec
## 1. System Architecture & The 3-Stage Pipeline
The KPL ecosystem splits compilation into three decoupled, deterministic steps. All components are written in strict ISO C99, requiring zero external dependencies, and must compile themselves from scratch using `slimcc` or `kefir` in under 10 seconds.

```text
[Multiple .kpl Source Files] 
       │
       ▼  (1) kpp (Preprocessor) -> Performs file-flattening & tree-shaking pass
[Single Flat Stream via stdout]
       │
       ▼  (2) kpl (Transpiler)   -> Emits deterministic System-V NASM text
[Raw .asm Text File]
       │
       ▼  (3) nasm & ld          -> Standard machine-code output assembly loops
[kpl_kernel.bin]
```

---

## 2. Preprocessor Reference (`kpp`)
The preprocessor flattens source hierarchies into a single token stream, executing function-level **dead-code elimination (tree-shaking)** before any assembly generation occurs.

### 2.1 Directives & Inclusion
*   `INCLUDE "<string_literal>"`: Merges the target file inline at the current stream position.
*   **Loop Mitigation**: `kpp` maintains a flat array `char visited_files[MAX_INCLUDES][MAX_PATH_LEN]`. If an input path matches a visited entry, it is skipped silently to prevent infinite cyclic preprocessing traps.

### 2.2 Tree-Shaking Syntax Indexing
*   **Pass 1**: `kpp` sweeps all files to discover definitions matching `PROC identifier`. It registers their filename, line number, and stream offset into a fixed internal database array.
*   **Pass 2**: `kpp` parses executions from the root entry file (`src/main.kpl`). It tracks active usage by capturing keywords matching `identifier(` or assembly blocks containing `call identifier`.
*   **Output Control**: Only `PROC` blocks flagged as actively called (`is_used == true`) are written to standard output (`stdout`).

### 2.3 Metadata Anchor Standard
Immediately prior to streaming out any approved code chunk, `kpp` must inject a standardized, machine-readable debugging text comment line:
```nasm
#;[<filename_string>]:[<procedure_or_struct_name>]:[<source_line_integer>]
```

---

## 3. Language & Transpiler Reference (`kpl`)

### 3.1 Primitives & Structural Types (`STRUCT`)
KPL recognizes exactly three native types: `u64` (8 bytes), `u32` (4 bytes), and `ptr` (8 bytes). 
```kpl
STRUCT Framebuffer {
    ptr address
    u64 width
    u64 height
    u32 pitch
}
```
*   **Compiler Action**: Computes structural sizes and saves absolute field offsets into memory (`address` = +0, `width` = +8, `height` = +16, `pitch` = +24). The `STRUCT` declaration itself does not emit text; it only populates a field-offset table consumed by 3.1.1.

#### 3.1.1 Field Access (`->`) and Indexing (`[N]`)
A variable declaration's type slot may name a `STRUCT` instead of a primitive. The compiler allocates the same 8-byte stack slot it would for `ptr`, and additionally tags the local with the matching struct's field table. A `STRUCT` field's type slot may do the same, tagging that field so `->` can continue through it.
```kpl
Framebuffer fb = 0
u64 w = fb->width
fb->width = 1920

Request req = 0
u64 n = req->response->count       ; chained: two hops
ptr first = req->response->items[0] ; chained, ending in a literal index
```
*   **Resolution**: a field-access/index token is resolved as a sequence of hops, left to right. The first segment (before the first `->`, if any) must be a declared local; each subsequent segment is looked up in the field table of whatever `STRUCT` the previous hop resolved to. A trailing `[N]` on any segment is parsed as a literal integer and contributes one more hop at byte offset `N * 8`, and ends the chain - the compiler does not track an indexed pointer's element type, so a further `->` after `[N]` is rejected at compile time rather than guessed at.
*   **Code-Generation Pattern**: every hop but the last re-dereferences a single scratch register; the last hop reads into (or writes from) the destination. An *n*-hop chain is always exactly *n* + 1 instructions - one base load, *n* - 1 intermediate dereferences, one final access:
```nasm
; req->response->items[0]  (three hops: response, items, [0])
mov r11, [rbp - 8]      ; load req
mov r11, [r11 + 40]     ; -> response
mov r11, [r11 + 16]     ; -> items
mov rax, [r11 + 0]      ; -> [0]
```
A single-hop chain (`fb->width`) is this same pattern with *n* = 1, unchanged from the two-instruction form it has always produced.
*   **Array index arithmetic happens at compile time.** `N` in `[N]` must be a constant integer literal; the compiler computes `N * 8` itself and emits it as a fixed displacement, exactly like a struct field's offset. There is no runtime multiply, and no variable-index form - `arr[i]` for a variable `i` is rejected, not silently supported with different codegen shape than a literal index would produce.
*   **Scratch Register**: `r11` is reserved by the compiler as the fixed pointer-dereference scratch register for every hop in a chain. It is never used as a call-argument register, a comparison/binary-operation register, or an assignment destination, so this never collides with surrounding codegen, regardless of chain length.
*   **Field type width**: the final hop's declared type determines the access width - a `u32` field is moved with the 32-bit register form (e.g. `mov eax, [r11 + 8]`), matching the same width-selection rule primitive `u32` variables already use. A `[N]` index is always pointer-width (8 bytes), since it has no declared type of its own.

### 3.2 Procedures & System V ABI Enforcement (`PROC`)
Procedures enforce a maximal barrier constraint of 6 input parameters. 
```kpl
PROC draw_rect(fb: ptr, x: u64, y: u64, w: u64, h: u64, color: u32) {
    ; Body execution logic
}
```
Arguments are mapped strictly left-to-right to the physical hardware execution registers mandated by the AMD64 System V specification:
1. `RDI`, 2. `RSI`, 3. `RDX`, 4. `RCX`, 5. `R8`, 6. `R9`.

#### Transpiler Output Schema:
```nasm
global draw_rect
draw_rect:
    push rbp
    mov rbp, rsp
    sub rsp, <calculated_16_byte_aligned_local_stack_frame_size>
    
    ; [Body content emitted here]
    
    mov rsp, rbp
    pop rbp
    ret
```

#### Cross-Translation-Unit Calls (`extern`)
The transpiler tracks every `PROC` name defined in the current translation unit, and every name ever used as a call target (both call statements and call-expressions, see below). Once the whole input has been processed, any called name that was never defined locally gets an `extern` declaration emitted for it, at the top of the output, ahead of `section .text`:
```nasm
extern get_framebuffer

section .text
...
```
This is what allows KPL code to call a routine written directly in NASM and linked in from a separate object file - a hand-written driver primitive, for example - using ordinary call syntax, with no `extern` declaration required in the KPL source itself. It applies only to genuine `identifier(args)` call syntax; a bare `call identifier` written by hand inside an `ASM` block (6.4) is raw, unparsed text and is **not** tracked by this mechanism - such a call still needs its own explicit `extern` line inside the `ASM` block.

#### Return Values (`RETURN`)
`RETURN` optionally accepts an expression, evaluated with the same machinery as a declaration's right-hand side (3.3), and placed in `RAX` immediately before the epilogue:
```nasm
; RETURN x + 5
mov rax, [rbp - 8]     ; x
mov rbx, 5
add rax, rbx
mov rsp, rbp
pop rbp
ret
```
A bare `RETURN` (no expression) emits only the epilogue, unchanged from earlier versions of this compiler.

**Calls as expressions**: the transpiler recognizes `identifier(args)` as a valid right-hand side wherever a declaration or assignment's RHS is expected, in addition to appearing as its own statement. Both forms emit identical call code (argument registers loaded, `call identifier`); the only difference is whether the caller stores `RAX` afterward:
```nasm
; u64 result = add_five(10)
mov rdi, 10
call add_five
mov [rbp - 16], rax
```
A call-expression is resolved as a single flat right-hand side, not composed with the `a OP b` grammar - `u64 x = f() + 1` is rejected by the same linearity check (3.3) that rejects `a + b * c`. A call used this way participates in the `extern` tracking above exactly like a call statement does.

### 3.3 Linear Assignment & Expressions
KPL bans nested algebraic expression trees entirely. All assignments must be flat, limiting code execution processing to a single operator.
```text
u64 alpha = beta + gamma   ; VALID: Compiled via single pass
u64 crash = a + b * c      ; INVALID: Triggers immediate compile crash
```

#### 3.3.1 Operator Code Generation
Every binary operator stages its right-hand operand through a fixed register before applying a single instruction against `RAX` - uniformly, whether the operand is a literal, a variable, or a `->` chain result, since this is a deterministic simple compiler and not an optimizer:
```nasm
; u64 c = a & b
mov rax, [rbp - 8]     ; a
mov rbx, [rbp - 16]    ; b
and rax, rbx
```
| Operator | Instruction | Staging register |
|---|---|---|
| `+` `-` `*` | `add` / `sub` / `imul` | `RBX` |
| `/` | `xor rdx, rdx` then `div` (unsigned) | `RBX` |
| `&` `\|` `^` | `and` / `or` / `xor` | `RBX` |
| `<<` `>>` | `shl` / `shr` (logical, unsigned) | `RCX` |

Shifts stage through `RCX` rather than `RBX` because x86-64 has no `shl reg, reg` form - a variable shift count is only encodable through `CL`. This is the one operator pair where the staging register differs; every other binary operator uses `RBX`.

Unary operators are a separate, single-operand code path - `RETURN`/declaration/assignment RHS parsing recognizes a 2-token `OP a` shape distinct from the 3-token `a OP b` shape, dispatching to its own small instruction table rather than a special case inside the binary-operator table, so that adding another unary operator later doesn't reshape the binary path:
```nasm
; u64 x = ~ flags
mov rax, [rbp - 8]     ; flags
not rax
```

### 3.4 Single-Pass Conditionals (`if` / `else`)
Branch execution uses a localized, runtime label stack instead of an AST.
```kpl
if status == 1 {
    u64 error = 0
} else {
    u64 error = 1
}
```
#### Code-Generation Label Output:
1.  **When `if` is reached**: Evaluates comparison statement (`cmp rax, 1`). Emits inverse condition jump targeting a unique sequential label (`jne .L_ELSE_0`). Pushes `.L_ELSE_0` and `.L_END_0` to the compiler stack tracker.
2.  **When `else` is reached**: Emits an unconditional escape jump (`jmp .L_END_0`), pops `.L_ELSE_0`, prints `.L_ELSE_0:` to standard output, and pushes `.L_END_0` back to track scope.
3.  **When closing `}` is reached**: Pops the remaining active label from the stack and prints it (`.L_END_0:`).

### 3.5 Inline Assembly Blocks (`ASM`)
When the parser encounters `ASM {`, the transpiler halts all high-level language validation rules. Raw characters are mirrored identically directly into the assembly pipeline output file stream until an equal matching balance of curly braces resolves to zero.
```kpl
ASM {
    cli
    mov rax, [fb]
    hlt
}
```

---

## 4. Environment & Scaffolding Tool (`kpl init`)
Executing `kpl init` within an empty shell directory forces the toolchain binary to automatically deploy a pristine, zero-config working framework containing exactly three files.

### 4.1 `linker.ld` (Higher-Half Kernel Memory Engine Mapping)
```linker
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kpl_main)
SECTIONS {
    . = 0xFFFFFFFF80000000; /* Maps directly to Limine higher-half rules */
    .text :   { *(.text*) }
    .rodata : { *(.rodata*) }
    .data :   { *(.data*) }
    .bss :    { *(.bss*) *(COMMON) }
}
```

### 4.2 Automated Engine Configuration (`Makefile`)
```makefile
all: kpl_kernel.bin

kpl_kernel.bin: src/main.kpl
	@mkdir -p build
	kpp src/main.kpl | kpl -o build/kernel.asm
	nasm -f elf64 build/kernel.asm -o build/kernel.o
	ld -nostdlib -z max-page-size=0x1000 -T linker.ld build/kernel.o -o kpl_kernel.bin

clean:
	rm -rf build kpl_kernel.bin
```

### 4.3 Baseline Sample Kernel (`src/main.kpl`)
```kpl
; Deployed via kpl init
PROC kpl_main() {
    ASM {
        cli
    .loop:
        hlt
        jmp .loop
    }
}
```
