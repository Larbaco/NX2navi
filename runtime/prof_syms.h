#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Writes `sym <address> <function>+0x<offset>` for each of the n addresses (any order, repeats allowed) that falls in
// the image loaded at base, from the COFF symbol table of the file at exe_path. Returns the lines written, -1 when the
// file has no symbol table or cannot be read.
int prof_syms_write(FILE *out, const char *exe_path, uint64_t base, const uint64_t *addr, size_t n);
