# kpl
Kernel Programming Language

## Why?
I ended up going down the rabbit hole of LFS (Linux From Sratch) twice. 
And I realised something. A stripped down Linux kernel config may compile in <6 minutes, but it is written in GNU C, which requires either GCC or Clang, both of which take 30-45 minutes to compile even on my 16 thread machine. I then discovered that alternative C compilers exist, like [slimcc](https://github.com/fuhsnn/slimcc) or [kefir](https://sr.ht/~jprotopopov/kefir/) (unmaintained) and I thought, why aren't we writing kernels in ISO C? The answer is GNU extensions which are necessary to allow for low level interaction with the hardware. As such, this led to me getting annoyed and realising something. No one (at least publicly) uses a DSL (Domain Specific Language) to program a kernel, that is where this idea of KPL was born, a specific programming language designed to allow a programmer to create a kernel with as little boilerplate as possible.

> **TL;DR:** GCC and Clang take too long to compile themselves

## Building

```sh
make          # builds ./kpp and ./kpl from src/
make test     # runs the regression suite in tests/ (needs nasm, ld;
              # qemu-system-x86_64, grub-mkrescue, and xorriso are used
              # for the boot-test fixtures if installed, and skipped
              # cleanly otherwise)
```

Both tools are strict ISO C99 with no dependencies beyond libc, so `CC` can point at anything - including [slimcc](https://github.com/fuhsnn/slimcc), the whole reason this project exists:

```sh
make CC=/path/to/slimcc
```

## Using it

```sh
mkdir myproject && cd myproject
/path/to/kpl init          # scaffolds linker.ld, Makefile, src/main.kpl
make                         # kpp -I. src/main.kpl | kpl -o build/kernel.asm,
                              # then nasm + ld
```

## Repo layout

```
src/kpp.c    preprocessor: INCLUDE flattening, PROC tree-shaking
src/kpl.c    transpiler: KPL -> NASM
spec/        LANGUAGE.md (the language) and COMPILER.md (how kpl implements it)
tests/       regression suite - see tests/run.sh
```

`spec/` is the source of truth for what KPL is supposed to do; `src/` is what it actually does. When they disagree, that's a bug in one or the other, not a judgment call.
