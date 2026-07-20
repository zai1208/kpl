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

### 2.2 Field Access (`->`)
A variable may be declared with a `STRUCT` name in place of a primitive type. Physically this is identical to a `ptr` (one 8-byte stack slot holding an address) - the struct name only tells the compiler which field table to resolve `->` against. A `STRUCT` field may itself be declared with another `STRUCT`'s name in place of a primitive type, the same way - it is physically an 8-byte pointer field, tagged with which struct's table its own `->` resolves against.
```kpl
STRUCT Response {
    u64 count
    ptr items
}
STRUCT Request {
    Response response
}

Request req = some_pointer_value
u64 n = req->response->count      ; VALID: chains through two structs
ptr first = req->response->items[0]  ; VALID: literal array index ends the chain
u64 idx = 2
ptr third = req->response->items[idx]  ; VALID: variable index, same rules
fb->width = 1920                    ; VALID: writes work the same as reads
```
*   **Placement**: `variable->field`, `variable->field->field`, and `variable[N]` / `variable->field[N]` are each a single token (no internal whitespace) and are valid anywhere a plain variable name is valid as an operand - the right-hand side of a declaration or assignment, an `if`/`loop` condition, or a `PROC` call argument.
*   **Chaining**: `->` may be repeated to walk through any number of nested `STRUCT`-typed fields, e.g. `a->b->c->d`. Each hop must itself have been declared with a `STRUCT` type; chaining onto a primitive field (`u64`/`u32`/`ptr`) is a compile-time error, since there is no field table to resolve the next hop against.
*   **Array indexing (`[N]`)**: a trailing `[N]` on the base variable or on any field in the chain indexes into what that pointer points to, as an array of pointer-sized (8-byte) elements: `base[N]` reads/writes the value at `base + N*8`. `N` may be a compile-time integer literal *or* any single resolvable operand (a variable, a register) - it may **not** itself be a `->`/`[...]` chain (`arr[obj->count]` is rejected; compute the index into a local first, `u64 idx = obj->count` then `arr[idx]`). `[N]` may only be the *last* step in a chain: the element type of an indexed array isn't tracked, so `arr[0]->field` is a compile-time error, not a silent guess.
*   **Linearity**: any field-access or index expression counts as one operand, the same as a bare variable name. `u64 w = fb->width` is a flat assignment; combining it with an operator still obeys the one-operator-per-line rule from 3.3 (`u64 total = fb->width + fb->height` is illegal for the same reason `a + b * c` is).
*   **Scope**: Field access and indexing resolve against local variables only. Neither is available inside `ASM` blocks' `[variable]` substitution (6.3) - that mechanism only matches a bare identifier, not an arrow or bracket expression.
*   **Unknown fields**: Referencing a field name not defined on the relevant `STRUCT` triggers a compile-time crash, consistent with the rest of the language's approach to undefined identifiers.

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

### 3.4 Operators
**Binary** (`a OP b`, one per line, per 3.3): `+` `-` `*` `/` `&` `|` `^` `<<` `>>`. Division and shifts are unsigned - every KPL type is unsigned (2), so `/` is an unsigned divide and `>>` is a logical (zero-filling) shift, never arithmetic/sign-extending.
```kpl
u64 masked = flags & 0x0F
u64 merged = a | b
u64 toggled = a ^ b
u64 doubled = x << 1
u64 halved = x >> 1
```
**Unary** (`OP a`, a single operand): `~` (bitwise NOT).
```kpl
u64 inverted = ~ flags
```
A unary operator requires a space before its operand, the same as every binary operator requires spaces around it (3.3) - `~flags` (no space) is not the same token as `~ flags` and will not parse as the NOT of `flags`.

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

### 4.4 Return Values (`RETURN`)
A `PROC` may exit early and/or return a value with `RETURN`, in one of two forms:
```kpl
PROC add_five(x: u64) {
    RETURN x + 5    ; computes the expression into RAX, then exits
}

PROC log_and_exit() {
    RETURN          ; exits immediately, RAX is whatever it already held
}
```
*   **Value form**: `RETURN <expr>` accepts anything valid as a declaration's right-hand side (3.3) - a single operand, or one `a OP b` expression - and places the result in `RAX`, the System V ABI's return-value register, immediately before the standard epilogue (4.3).
*   **Void form**: bare `RETURN` runs the epilogue immediately without touching `RAX`. This is the only form available in earlier versions of this spec; the value form is additive.
*   **Calling a PROC for its value**: a call `identifier(args)` may be used directly as a declaration or assignment's right-hand side:
    ```kpl
    u64 result = add_five(10)
    ```
    A call used this way is one flat operand - the same tier as a bare variable name - and **cannot** be combined with an operator on the same line. `u64 x = add_five(10) + 1` is a linearity violation (3.3) for the same reason `u64 x = a + b * c` is; write it as two lines instead.
*   **Falling off the end**: every `PROC`'s closing `}` emits the same epilogue regardless of whether `RETURN` was used. A `PROC` whose callers expect a value but which reaches `}` without an explicit `RETURN <expr>` returns whatever `RAX` happened to hold - this is a real sharp edge, not a checked error, consistent with the rest of the language's minimal, unvalidated approach to control flow.

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

### 5.3 Single-Pass Loops (`loop`)
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
A `call` written this way is raw, unparsed text - unlike ordinary `identifier(args)` call syntax (4.4), it does not benefit from automatic `extern` declaration (see COMPILER.md 3.2). If the target is defined in a different translation unit, add `extern target_name` as its own line inside the `ASM` block.
***
