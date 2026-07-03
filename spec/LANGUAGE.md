# KPL Language spec

## 1. Syntax Overview & Lexical Elements

### 1.1 Source Files
KPL source files must use the `.kpl` file extension and be encoded in standard ASCII text.

### 1.2 Comments
Any text on a line following a semicolon `;` is a comment. The compiler strips these during parsing but passes them directly into the output `.asm` file to aid debugging.
```kpl
; This is a global comment
u64 boot_magic = 0x2BADB002 ; This is an inline comment
```

### 1.3 Identifiers
Identifiers designate names for procedures, structures, structural fields, and variables. They are case-sensitive and must match the regular expression: `[a-zA-Z_][a-zA-Z0-9_]*`.

---

## 2. Type System & Memory Data Layout

KPL recognizes exactly three primitive types, each mapping directly to a 64-bit or 32-bit hardware storage register configuration:
*   `u64`: 64-bit unsigned integer.
*   `u32`: 32-bit unsigned integer.
*   `ptr`: 64-bit memory address pointer.

### 2.1 Structured Data Types (`STRUCT`)
Structures are compile-time memory layout templates. They define data offsets but emit no assembly instructions.
```kpl
STRUCT Framebuffer {
    ptr address
    u64 width
    u64 height
    u32 pitch
}
```
*   **Compilation Rule**: The compiler records the absolute byte offset of each field relative to the start of the structure (`address` = +0, `width` = +8, `height` = +16, `pitch` = +24). 
*   **Memory Realignment**: Fields are packed tightly using standard 64-bit / 32-bit alignment limits.

---

## 3. Variable Management & Linearity Enforcement

### 3.1 Global Variables
Variables declared outside the scope of a `PROC` block are allocated globally in the executable's `.data` section.
```kpl
u64 global_counter = 0
ptr physical_memory_map = 0
```

### 3.2 Local Variables
Variables declared inside a `PROC` block are allocated slot positions relative to the Base Pointer (`RBP`) on the active CPU stack frame.

### 3.3 Strict Linearity Constraint
To completely eliminate the need for complex, heavy Abstract Syntax Trees (ASTs), all mathematical and assignment expressions are limited to a maximum of **one operator per line**.
```kpl
u64 target = alpha          ; VALID: Flat assignment
u64 total = alpha + beta    ; VALID: Single-operator linear expression
u64 crash = a + b * c       ; ILLEGAL: Triggers a compile-time crash
```
Complex math operations must be explicitly broken across multiple linear steps by the programmer:
```kpl
u64 temp = b * c
u64 total = a + temp
```

---

## 4. System V AMD64 ABI Enforcement (`PROC`)

Procedures (`PROC`) represent explicit code memory blocks. KPL statically enforces the AMD64 System V Calling Convention.

```kpl
PROC draw_pixel(fb: ptr, x: u64, y: u64, color: u32) {
    ; Code logic here
}
```

### 4.1 Parameter Limitations
Procedures are restricted to a **maximum of 6 parameters**. 

### 4.2 Register Allocation
Arguments are bound strictly left-to-right to the physical CPU hardware registers mandated by the System V ABI specification:
1.  `RDI` (Argument 1)
2.  `RSI` (Argument 2)
3.  `RDX` (Argument 3)
4.  `RCX` (Argument 4)
5.  `R8`  (Argument 5)
6.  `R9`  (Argument 6)

### 4.3 Deterministic Code Generation Frame
Every `PROC` block automatically triggers the output of standard entry prologue and exit epilogue assembly sequences:
```nasm
global draw_pixel
draw_pixel:
    push rbp
    mov rbp, rsp
    sub rsp, <aligned_local_stack_size> ; Maintained on a strict 16-byte boundary
    
    ; [Procedure content generated here]
    
    mov rsp, rbp
    pop rbp
    ret
```

---

## 5. Control Flow & Branching Operations

KPL does not include keywords for loops (`while`, `for`). Programmers manage loops manually inside inline assembly blocks using CPU jump flags. However, KPL natively implements single-pass linear conditional branches (`if` and `else`).

### 5.1 Single-Pass Conditionals
Conditionals match a strict, flat structure containing a single comparison operator: `==`, `!=`, `<`, or `>`.
```kpl
if boot_id == 1 {
    u64 screen_width = 1920
} else {
    u64 screen_width = 1024
}
```

### 5.2 Single-Pass Branching Pipeline Logic
The transpiler tracks active code blocks using a local label stack to resolve conditional targets on the fly without an AST:
1.  **On matching `if`**: Emits an inverse condition jump instruction targeting a unique sequential label (`jne .L_ELSE_0`). Pushes tracking pointers to the compiler stack framework.
2.  **On matching `else`**: Emits an unconditional branch escape (`jmp .L_END_0`), outputs the active label identifier (`.L_ELSE_0:`), and updates the stack tracking state.
3.  **On matching the closing brace `}`**: Pops the remaining active label identifier from the internal tracking stack and prints it to the output file (`.L_END_0:`).
## 5.3 Single-Pass Loops (`loop`)
Loops execute a block of code repeatedly while a strict, flat conditional expression is true. The loop condition uses a single comparison operator: `==`, `!=`, `<`, or `>`.

loop rax < 10 {
    ; Loop body logic here
    rax = rax + 1
}

---

## 6. Inline Assembly Blocks (`ASM`)

The `ASM` block bridges KPL directly to raw hardware execution registers, bypassing high-level code parsing entirely.

```kpl
PROC disable_interrupts() {
    ASM {
        cli
    }
}
```

### 6.1 Raw Character Streaming
When the compiler encounters `ASM {`, high-level type and syntax validation rules suspend completely. Raw characters are mirrored identically directly into the output `.asm` file stream.

### 6.2 Brace Counting Closure
Streaming remains active until a tracking brace counter state resolves to zero, signaling the structural termination of the assembly encapsulation block.

### 6.3 Variable Referencing Inside ASM
Local variables can be read or modified inside an `ASM` block by enclosing the variable identifier in square brackets. The transpiler maps these directly to their stack pointer offsets:
```text
mov rax, [my_local_var]   ---> Transpiles to --->   mov rax, [rbp - 8]
```

### 6.4 Procedure Invocations
Programmers can execute any KPL procedure from inside an assembly block using the native NASM `call` instruction. The programmer is manually responsible for staging the correct registers (`RDI`, `RSI`, `RDX`, etc.) before initiating the call tracking transition:
```nasm
ASM {
    mov rdi, [fb_ptr]
    call clear_screen
}
```
***
