# kpl
Kernel Programming Language

## Why?
I ended up going down the rabbit hole of LFS (Linux From Sratch) twice. 
And I realised something. A stripped down Linux kernel config may compile in <6 minutes, but it is written in GNU C, which requires either GCC or Clang, both of which take 30-45 minutes to compile even on my 16 thread machine. I then discovered that alternative C compilers exist, like [slimcc](https://github.com/fuhsnn/slimcc) or [kefir](https://sr.ht/~jprotopopov/kefir/) (unmaintained) and I thought, why aren't we writing kernels in ISO C? The answer is GNU extensions which are necessary to allow for low level interaction with the hardware. As such, this led to me getting annoyed and realising something. No one (at least publicly) uses a DSL (Domain Specific Language) to program a kernel, that is where this idea of KPL was born, a specific programming language designed to allow a programmer to create a kernel with as little boilerplate as possible.

> **TL;DR:** GCC and Clang take too long to compile themselves
