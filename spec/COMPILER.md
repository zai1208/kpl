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
*   **Compiler Action**: Computes structural sizes and saves absolute field offsets into memory (`address` = +0, `width` = +8, `height` = +16, `pitch` = +24). It does not emit text.

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

### 3.3 Linear Assignment & Expressions
KPL bans nested algebraic expression trees entirely. All assignments must be flat, limiting code execution processing to a single operator.
```text
u64 alpha = beta + gamma   ; VALID: Compiled via single pass
u64 crash = a + b * c      ; INVALID: Triggers immediate compile crash
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
