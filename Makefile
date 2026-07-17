# Builds the KPL toolchain itself (kpp + kpl).
#
# Per spec/COMPILER.md section 1, both tools are strict ISO C99 with zero
# external dependencies and should self-compile in well under 10 seconds -
# true for gcc/clang and for slimcc (https://github.com/fuhsnn/slimcc) or
# kefir alike. Override CC to try a different compiler, e.g.:
#
#   make CC=/path/to/slimcc
#
# Once built, put this directory on PATH (or copy kpp/kpl somewhere that
# is) so that `kpl init`-generated project Makefiles can find them as
# bare `kpp`/`kpl` commands.

CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra

all: kpp kpl

kpp: src/kpp.c
	$(CC) $(CFLAGS) -o $@ $<

kpl: src/kpl.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	@./tests/run.sh

clean:
	rm -f kpp kpl

.PHONY: all test clean
