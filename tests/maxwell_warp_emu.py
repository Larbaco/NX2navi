"""A 32-lane Maxwell warp emulator for checking the SM86 tensor lowering on the CPU.

tools/xlate/AmpereLowering.cs rewrites SM86 instructions as Maxwell (64-bit) instructions, and the patched shader
translator (build/xlate/ryujinx/src/Ryujinx.Graphics.Shader) turns those into SPIR-V. This file executes the Maxwell
words the way that translator does: fields are decoded exactly as its Decoders/InstDecoders.cs, opcodes are
matched with the bit patterns of Decoders/InstTable.cs, and each instruction does what its Instructions/*.cs
emit for it (not what a real Maxwell GPU does where the two differ; see "Translator semantics" below). With it, the result
of one lowered SM86 instruction (HMMA, LDSM, MOVM, HFMA2.RELU, ...) can be compared with vectors captured on real
hardware without running the game.

State per lane: R0-R254 (RZ = R255 reads 0, writes are dropped), P0-P6 (PT = P7 is true, writes dropped), the lane id,
local memory; per warp: shared memory. Every lane is active at the start; a guard (bits 16-19: predicate 16-18, inverted
19) masks the writes of one instruction, EXIT retires lanes. Branches must be uniform across the active lanes.

Translator semantics this follows (and real hardware may not):
  * Half-precision arithmetic (HADD2, HMUL2, HFMA2) works in F32 on the unpacked halves and rounds to F16 once at the
    end (InstEmitFloatArithmetic EmitHadd2Hmul2 / EmitHfma2, InstEmitHelper GetHalfPacked: PackHalf2x16).
    HFMA2 is therefore an F32 fused multiply-add rounded to F32, then to F16.
  * That rounding is skipped when the packed value goes straight into another unpack: Optimizer.PropagatePack
    turns UnpackHalf2x16(PackHalf2x16(a, b)) into a (or b). So F2F.F16.F32 then F2F.F32.F16 of the same register, or
    HFMA2 feeding HFMA2, passes the F32 value on unrounded; an integer instruction (SHFL, LOP, ISCADD, ...) in between
    keeps the rounding, a MOV does not. Modelled per register (Warp.halves); a guarded write and a branch end it.
  * FTZ, FMZ and rounding-mode fields of FFMA / FMUL / FADD / HFMA2 / HMUL2 are ignored, as the translator ignores them.
    Denormals are kept (IEEE), saturation is clamp(x, 0, 1) with NaN giving 0 (GLSL FClamp as min(max(x, 0), 1) on
    a GPU whose min/max return the non-NaN operand).
  * PRMT is not implemented in the translator (1.3.2): InstEmit.PrmtR/PrmtI/PrmtC/PrmtRc only log "not implemented", so the
    destination keeps its old value. That is the default here (prmt="ryujinx"); prmt="maxwell" gives the hardware
    byte permute instead, to see what the lowering intended.
  * SHFL follows the translator's HelperFunctionManager GenerateShuffle* helpers (clamp and segment mask from operand C, the
    lane's own value where the source lane is out of range).
  * LDS / LDL read the 32-bit word at (address >> 2) and extract a sub-word at (address & 3) * 8 (InstEmitMemory
    EmitLoad); STS of 8 and 16 bits stores those bytes at the address.
  * Condition codes (.CC, .X) are not modelled: an instruction that writes or reads them raises.
  * NaN results carry numpy's payload and sign. Neither is defined in SPIR-V, so result comparisons (same_value) let
    a NaN match any NaN for float results; see "comparing results".

Library use:
  regs = run(words, regs_in, smem=None)        # words: Maxwell 64-bit words; returns a (32, 256) uint32 array
  warp = run_warp(words, regs_in, smem=None)   # the whole Warp (regs, preds, smem, warnings)

CLI (vectors written by the tensor ground-truth capture, see main()):
  python tests/maxwell_warp_emu.py <vectors.json> [--xlate EXE] [--prmt ryujinx|maxwell] [--case NAME] [--trace]
                                   [--fill HEX] [--exact]
tests/tensor_lowering_check.py runs the full check of the tensor lowering on top of this (guards, untouched registers,
several initial fills).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_XLATE = ROOT.parent.parent / "build" / "xlate" / ("nx2navi_xlate.exe" if os.name == "nt" else "nx2navi_xlate")

LANES = 32
RZ = 255
PT = 7
U32 = np.uint32
F32 = np.float32
F16 = np.float16


class UnknownOpcode(Exception):
    """A Maxwell word this emulator has no decoder entry for."""


class Unsupported(Exception):
    """A known instruction in a mode or form the emulator does not implement."""


# ---- bit-level helpers --------------------------------------------------------------------------------------------

def u32(x) -> np.ndarray:
    """A lane vector (or broadcast scalar) as uint32, wrapping Python ints to 32 bits."""
    if isinstance(x, (int, np.integer)):
        return np.full(LANES, int(x) & 0xFFFFFFFF, dtype=U32)
    return np.asarray(x).astype(U32, copy=True)


def sext(value: int, bits: int) -> int:
    """Sign-extends a bits-wide field to a Python int."""
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def as_f32(u: np.ndarray) -> np.ndarray:
    return u32(u).view(F32)


def from_f32(f: np.ndarray) -> np.ndarray:
    return np.asarray(f, dtype=F32).view(U32).copy()


def half_lo(u: np.ndarray) -> np.ndarray:
    """UnpackHalf2x16Low: the low 16 bits as F16, widened to F32 (exact)."""
    return (u32(u) & U32(0xFFFF)).astype(np.uint16).view(F16).astype(F32)


def half_hi(u: np.ndarray) -> np.ndarray:
    """UnpackHalf2x16High."""
    return (u32(u) >> U32(16)).astype(np.uint16).view(F16).astype(F32)


def pack_half(lo: np.ndarray, hi: np.ndarray) -> np.ndarray:
    """PackHalf2x16: each F32 rounded to F16 (round to nearest even, numpy's conversion), lo in bits 0-15."""
    l = np.asarray(lo, dtype=F32).astype(F16).view(np.uint16).astype(U32)
    h = np.asarray(hi, dtype=F32).astype(F16).view(np.uint16).astype(U32)
    return l | (h << U32(16))


def fma_f32(a: np.ndarray, b: np.ndarray, c: np.ndarray) -> np.ndarray:
    """F32 fused multiply-add, rounded once (FPFusedMultiplyAdd -> GLSL.std.450 Fma, fused on the target GPUs).

    a * b is exact in F64 (2 x 24 significant bits). The F64 sum s may be inexact; TwoSum gives its error e exactly
    (s + e == a * b + c). Rounding s to F32 is then correct except when s lies exactly halfway between two F32
    values, where the sign of e decides the direction. F32 halfway points are F64 values, so no other case can
    double-round."""
    a64 = np.asarray(a, dtype=F32).astype(np.float64)
    b64 = np.asarray(b, dtype=F32).astype(np.float64)
    c64 = np.asarray(c, dtype=F32).astype(np.float64)
    p = a64 * b64
    s = p + c64
    bb = s - p
    e = (p - (s - bb)) + (c64 - bb)
    r = s.astype(F32)
    # The F32 neighbour of r on the side of s.
    toward = np.nextafter(r, np.where(s > r.astype(np.float64), F32(np.inf), F32(-np.inf)).astype(F32))
    mid = (r.astype(np.float64) + toward.astype(np.float64)) * 0.5
    fix = np.isfinite(s) & np.isfinite(r) & np.isfinite(toward) & (e != 0) & (s == mid)
    if fix.any():
        # At a tie, the exact value lies on the side of e: take the neighbour when e points towards it.
        up = (toward.astype(np.float64) > r.astype(np.float64))
        take = fix & (up == (e > 0))
        r = np.where(take, toward, r).astype(F32)
    return r


def saturate(x: np.ndarray) -> np.ndarray:
    """FPSaturate: clamp to [0, 1], NaN to 0 (fmax/fmin return the non-NaN operand)."""
    return np.fmin(np.fmax(np.asarray(x, dtype=F32), F32(0)), F32(1)).astype(F32)


def abs_neg(x: np.ndarray, absolute: bool, negate: bool) -> np.ndarray:
    """FPAbsNeg: abs first, then negate (sign-bit operations in SPIR-V; numpy's abs/negative act on the sign too)."""
    x = np.asarray(x, dtype=F32)
    if absolute:
        x = np.abs(x)
    if negate:
        x = np.negative(x)
    return x


class Ins:
    """One Maxwell word with the field accessors InstDecoders.cs uses."""

    def __init__(self, word: int, index: int):
        self.w = word
        self.index = index

    def bits(self, offset: int, count: int) -> int:
        return (self.w >> offset) & ((1 << count) - 1)

    def bit(self, offset: int) -> bool:
        return (self.w >> offset) & 1 == 1

    # Fields shared by most decoders.
    @property
    def rd(self): return self.bits(0, 8)
    @property
    def ra(self): return self.bits(8, 8)
    @property
    def rb(self): return self.bits(20, 8)
    @property
    def rc(self): return self.bits(39, 8)
    @property
    def imm20(self):   # InstDecoders Imm20: bits 20-38, bit 19 from bit 56
        return ((self.w >> 37) & 0x80000) | ((self.w >> 20) & 0x7FFFF)
    @property
    def imm20_sint(self): return sext(self.imm20, 20)   # InstEmitHelper Imm20ToSInt
    @property
    def imm20_float(self): return (self.imm20 << 12) & 0xFFFFFFFF   # Imm20ToFloat
    @property
    def imm32(self): return (self.w >> 20) & 0xFFFFFFFF
    @property
    def imm24_sint(self): return sext(self.bits(20, 24), 24)

    def __repr__(self):
        return f"[{self.index}] {self.w:016x}"


# ---- warp state ---------------------------------------------------------------------------------------------------

class Warp:
    def __init__(self, smem: bytes | None = None, smem_size: int = 0x10000, local_size: int = 0x2000,
                 cbufs: dict | None = None, prmt: str = "ryujinx", fill: int = 0, trace: bool = False):
        # Registers not given by the caller start as `fill` (SPIR-V locals are undefined unless the job zeroes them;
        # a poison value such as 0x7fc0dead shows reads of registers the lowering never wrote).
        self.r = np.full((LANES, 256), fill & 0xFFFFFFFF, dtype=U32)
        self.r[:, RZ] = 0
        self.p = np.zeros((LANES, 8), dtype=bool)
        self.p[:, PT] = True
        self.smem = bytearray(smem) if smem is not None else bytearray(smem_size)
        self.local = [bytearray(local_size) for _ in range(LANES)]
        self.cbufs = {int(k): bytes(v) for k, v in (cbufs or {}).items()}
        self.lane = np.arange(LANES, dtype=U32)
        self.active = np.ones(LANES, dtype=bool)
        if prmt not in ("ryujinx", "maxwell"):
            raise ValueError(f"prmt mode {prmt!r}")
        self.prmt = prmt
        self.trace = trace
        self.warnings: list[str] = []
        # The translator's Optimizer.PropagatePack turns an UnpackHalf2x16 of a PackHalf2x16 result into the pack's F32
        # operand, so that half is never rounded to F16. Per lane and register: the F32 halves of the pack that wrote
        # the register (sh_ok), kept until the register is written otherwise. A write under a guard ends it in every
        # lane: the translator puts the instruction in an if-block and merges old and new value in a phi, which is no pack.
        self.sh_lo = np.zeros((LANES, 256), dtype=F32)
        self.sh_hi = np.zeros((LANES, 256), dtype=F32)
        self.sh_ok = np.zeros((LANES, 256), dtype=bool)
        self.unconditional = True   # the current instruction's guard is PT (run_warp sets it)

    # Register file access: R(n) reads a lane vector, W(n, v, mask) writes the lanes in mask. halves: the F32 values
    # a PackHalf2x16 rounded into v (see sh_ok); copy_from: v is a copy of that register (MOV), pack and all.
    def R(self, n: int) -> np.ndarray:
        if n == RZ:
            return np.zeros(LANES, dtype=U32)
        return self.r[:, n].copy()

    def W(self, n: int, v, mask: np.ndarray, halves=None, copy_from: int | None = None):
        if n == RZ:
            return
        v = u32(v)
        self.r[mask, n] = v[mask]
        if not self.unconditional:
            self.sh_ok[:, n] = False
        elif halves is not None:
            self.sh_lo[mask, n] = np.broadcast_to(np.asarray(halves[0], dtype=F32), (LANES,))[mask]
            self.sh_hi[mask, n] = np.broadcast_to(np.asarray(halves[1], dtype=F32), (LANES,))[mask]
            self.sh_ok[mask, n] = True
        elif copy_from is not None and copy_from != RZ:
            for arr in (self.sh_lo, self.sh_hi, self.sh_ok):
                arr[mask, n] = arr[mask, copy_from]
        else:
            self.sh_ok[mask, n] = False

    def halves(self, n: int):
        """UnpackHalf2x16Low / High of register n as F32, after PropagatePack (the pack's operand where one wrote n)."""
        u = self.R(n)
        lo, hi = half_lo(u), half_hi(u)
        if n != RZ:
            ok = self.sh_ok[:, n]
            lo = np.where(ok, self.sh_lo[:, n], lo).astype(F32)
            hi = np.where(ok, self.sh_hi[:, n], hi).astype(F32)
        return lo, hi

    def P(self, n: int, inv: bool = False) -> np.ndarray:
        v = np.ones(LANES, dtype=bool) if n == PT else self.p[:, n].copy()
        return ~v if inv else v

    def WP(self, n: int, v, mask: np.ndarray):
        if n == PT:
            return
        v = np.broadcast_to(np.asarray(v, dtype=bool), (LANES,))
        self.p[mask, n] = v[mask]

    def cbuf_word(self, slot: int, word_offset: int) -> np.ndarray:
        """c[slot][word_offset * 4] (InstDecoders CbufOffset counts words)."""
        data = self.cbufs.get(slot)
        at = word_offset * 4
        if data is None or at + 4 > len(data):
            raise Unsupported(f"constant buffer c[{slot}][0x{at:x}] not provided")
        return u32(int.from_bytes(data[at:at + 4], "little"))


# ---- instruction semantics ----------------------------------------------------------------------------------------
# Each handler takes (warp, ins, mask): mask = the active lanes whose guard holds. Lanes outside mask must not change.

def _no_cc(i: Ins, cc_bit: int | None, x_bit: int | None, name: str):
    if cc_bit is not None and i.bit(cc_bit):
        raise Unsupported(f"{name}.CC (condition codes not modelled) {i}")
    if x_bit is not None and i.bit(x_bit):
        raise Unsupported(f"{name}.X (carry not modelled) {i}")


def op_nop(w, i, m):
    pass


def op_mov_r(w, i, m):            # InstEmitMove MovR: SrcA in bits 20-27; a Copy, which PropagateCopy sees through
    w.W(i.rd, w.R(i.rb), m, copy_from=i.rb)


def op_mov_i(w, i, m):            # MovI: GetSrcImm(op.Imm20), the raw 20-bit field (not shifted or sign-extended)
    w.W(i.rd, u32(i.imm20), m)


def op_mov_c(w, i, m):
    w.W(i.rd, w.cbuf_word(i.bits(34, 5), i.bits(20, 14)), m)


def op_mov32i(w, i, m):
    w.W(i.rd, u32(i.imm32), m)


def _iadd(w, i, m, a, b, avg):    # InstEmitIntegerArithmetic EmitIadd: AvgMode 1 negates B, 2 negates A
    if avg == 2:
        a = U32(0) - a
    if avg == 1:
        b = U32(0) - b
    w.W(i.rd, a + b, m)


def op_iadd_r(w, i, m):
    _no_cc(i, 47, 43, "IADD")
    _iadd(w, i, m, w.R(i.ra), w.R(i.rb), i.bits(48, 2))


def op_iadd_i(w, i, m):
    _no_cc(i, 47, 43, "IADD")
    _iadd(w, i, m, w.R(i.ra), u32(i.imm20_sint), i.bits(48, 2))


def op_iadd32i(w, i, m):
    _no_cc(i, 52, 53, "IADD32I")
    _iadd(w, i, m, w.R(i.ra), u32(i.imm32), i.bits(55, 2))


def _iadd3(w, i, m, a, b, c, lrs, parts):
    def extend(v, part):          # EmitIadd3 Extend: HalfSelect 1 = low 16 bits, 2 = high 16 bits
        if part == 1:
            return v & U32(0xFFFF)
        if part == 2:
            return v >> U32(16)
        return v
    a, b, c = (extend(v, p) for v, p in zip((a, b, c), parts))
    if i.bit(51):
        a = U32(0) - a
    if i.bit(50):
        b = U32(0) - b
    if i.bit(49):
        c = U32(0) - c
    res = a + b
    if lrs == 2:
        res = res << U32(16)
    elif lrs == 1:
        res = res >> U32(16)
    w.W(i.rd, res + c, m)


def op_iadd3_r(w, i, m):
    _no_cc(i, 47, 48, "IADD3")
    _iadd3(w, i, m, w.R(i.ra), w.R(i.rb), w.R(i.rc), i.bits(37, 2), (i.bits(35, 2), i.bits(33, 2), i.bits(31, 2)))


def op_iadd3_i(w, i, m):
    _no_cc(i, 47, 48, "IADD3")
    _iadd3(w, i, m, w.R(i.ra), u32(i.imm20_sint), w.R(i.rc), 0, (0, 0, 0))


def _iscadd(w, i, m, a, b, shift, avg):   # EmitIscadd: (A << shift), then the AvgMode negations, then A + B
    a = a << U32(shift)
    if avg == 2:
        a = U32(0) - a
    if avg == 1:
        b = U32(0) - b
    w.W(i.rd, a + b, m)


def op_iscadd_r(w, i, m):
    _no_cc(i, 47, None, "ISCADD")
    _iscadd(w, i, m, w.R(i.ra), w.R(i.rb), i.bits(39, 5), i.bits(48, 2))


def op_iscadd_i(w, i, m):
    _no_cc(i, 47, None, "ISCADD")
    _iscadd(w, i, m, w.R(i.ra), u32(i.imm20_sint), i.bits(39, 5), i.bits(48, 2))


def op_iscadd32i(w, i, m):
    _no_cc(i, 52, None, "ISCADD32I")
    _iscadd(w, i, m, w.R(i.ra), u32(i.imm32), i.bits(53, 5), 0)


def _shl(w, i, m, a, b):          # InstEmitShift EmitShl: .M masks the count to 5 bits, else counts >= 32 give 0
    _no_cc(i, 47, 43, "SHL")
    if i.bit(39):
        b = b & U32(31)
        w.W(i.rd, a << b, m)
    else:
        w.W(i.rd, np.where(b < 32, a << (b & U32(31)), U32(0)), m)


def _shr(w, i, m, a, b):          # EmitShr: .BREV (40) reverses A first, .S32 (48), .M (39)
    _no_cc(i, 47, None, "SHR")
    if i.bits(43, 2):
        raise Unsupported(f"SHR.X {i}")
    if i.bit(40):
        a = u32([int(f"{int(v):032b}"[::-1], 2) for v in a])
    signed = i.bit(48)
    if i.bit(39):
        b = b & U32(31)
    sh = b & U32(31)
    res = (a.view(np.int32) >> sh.astype(np.int32)).view(U32) if signed else a >> sh
    if not i.bit(39):
        big = (a.view(np.int32) >> 31).view(U32) if signed else np.zeros(LANES, dtype=U32)
        res = np.where(b < 32, res, big)
    w.W(i.rd, res, m)


def op_shl_r(w, i, m): _shl(w, i, m, w.R(i.ra), w.R(i.rb))
def op_shl_i(w, i, m): _shl(w, i, m, w.R(i.ra), u32(i.imm20_sint))
def op_shr_r(w, i, m): _shr(w, i, m, w.R(i.ra), w.R(i.rb))
def op_shr_i(w, i, m): _shr(w, i, m, w.R(i.ra), u32(i.imm20_sint))


def _lop_pred(w, i, m, res, pred_op, dest_pred):   # EmitLopPredWrite
    if dest_pred == PT:
        return
    v = {0: np.zeros(LANES, bool), 1: np.ones(LANES, bool), 2: res == 0, 3: res != 0}[pred_op]
    w.WP(dest_pred, v, m)


def _lop(w, i, m, a, b, logic, inv_a, inv_b, pred_op, dest_pred):   # InstEmitIntegerLogical EmitLop
    if inv_a:
        a = ~a
    if inv_b:
        b = ~b
    res = {0: a & b, 1: a | b, 2: a ^ b, 3: b}[logic]
    _lop_pred(w, i, m, res, pred_op, dest_pred)
    w.W(i.rd, res, m)


def op_lop_r(w, i, m):
    _no_cc(i, 47, 43, "LOP")
    _lop(w, i, m, w.R(i.ra), w.R(i.rb), i.bits(41, 2), i.bit(39), i.bit(40), i.bits(44, 2), i.bits(48, 3))


def op_lop_i(w, i, m):
    _no_cc(i, 47, 43, "LOP")
    _lop(w, i, m, w.R(i.ra), u32(i.imm20_sint), i.bits(41, 2), i.bit(39), i.bit(40), i.bits(44, 2), i.bits(48, 3))


def op_lop32i(w, i, m):           # Lop32i: no predicate output (DestPred PT)
    _no_cc(i, 52, 57, "LOP32I")
    _lop(w, i, m, w.R(i.ra), u32(i.imm32), i.bits(53, 2), i.bit(55), i.bit(56), 0, PT)


def _lop3(a, b, c, lut):
    """The LOP3 truth table (Lop3Expression.GetFromTruthTable): bit (a << 2 | b << 1 | c) of lut, per bit."""
    res = np.zeros(LANES, dtype=U32)
    for k in range(8):
        if lut >> k & 1:
            ta = a if k & 4 else ~a
            tb = b if k & 2 else ~b
            tc = c if k & 1 else ~c
            res |= ta & tb & tc
    return res


def op_lop3_r(w, i, m):
    _no_cc(i, 47, 38, "LOP3")
    res = _lop3(w.R(i.ra), w.R(i.rb), w.R(i.rc), i.bits(28, 8))
    _lop_pred(w, i, m, res, i.bits(36, 2), i.bits(48, 3))
    w.W(i.rd, res, m)


def op_lop3_i(w, i, m):
    _no_cc(i, 47, 57, "LOP3")
    w.W(i.rd, _lop3(w.R(i.ra), u32(i.imm20_sint), w.R(i.rc), i.bits(48, 8)), m)


def _prmt(w, i, m, a, sel, c):
    if w.prmt == "ryujinx":
        # The translator's InstEmit.PrmtR / PrmtI: "not implemented", nothing is emitted, the destination keeps its value.
        w.warnings.append(f"PRMT at {i} is a no-op in Ryujinx 1.3.2 (destination R{i.rd} unchanged)")
        return
    if i.bits(48, 4):
        raise Unsupported(f"PRMT mode {i.bits(48, 4)} {i}")
    res = np.zeros(LANES, dtype=U32)
    src = [(a >> U32(8 * k)) & U32(0xFF) for k in range(4)] + [(c >> U32(8 * k)) & U32(0xFF) for k in range(4)]
    for byte in range(4):
        nib = (sel >> U32(4 * byte)) & U32(0xF)
        val = np.zeros(LANES, dtype=U32)
        for k in range(8):
            val = np.where((nib & U32(7)) == k, src[k], val)
        val = np.where((nib & U32(8)) != 0, np.where(val & U32(0x80), U32(0xFF), U32(0)), val)
        res |= val << U32(8 * byte)
    w.W(i.rd, res, m)


def op_prmt_r(w, i, m): _prmt(w, i, m, w.R(i.ra), w.R(i.rb), w.R(i.rc))
def op_prmt_i(w, i, m): _prmt(w, i, m, w.R(i.ra), u32(i.imm20_sint), w.R(i.rc))


def op_sel_r(w, i, m):            # InstEmitMove EmitSel: P ? A : B
    p = w.P(i.bits(39, 3), i.bit(42))
    w.W(i.rd, np.where(p, w.R(i.ra), w.R(i.rb)), m)


def op_sel_i(w, i, m):
    p = w.P(i.bits(39, 3), i.bit(42))
    w.W(i.rd, np.where(p, w.R(i.ra), u32(i.imm20_sint)), m)


def _isetp(w, i, m, a, b):        # InstEmitIntegerComparison EmitIsetp (not extended)
    if i.bit(43):
        raise Unsupported(f"ISETP.X {i}")
    cmp, signed = i.bits(49, 3), i.bit(48)
    x, y = (a.view(np.int32), b.view(np.int32)) if signed else (a, b)
    r = {0: np.zeros(LANES, bool), 1: x < y, 2: x == y, 3: x <= y, 4: x > y, 5: x != y, 6: x >= y,
         7: np.ones(LANES, bool)}[cmp]
    pred = w.P(i.bits(39, 3), i.bit(42))
    op = i.bits(45, 2)
    def comb(v):
        if op == 0:
            return v & pred
        if op == 1:
            return v | pred
        if op == 2:
            return v ^ pred
        raise Unsupported(f"ISETP bool op {op} {i}")
    w.WP(i.bits(3, 3), comb(r), m)
    w.WP(i.bits(0, 3), comb(~r), m)


def op_isetp_r(w, i, m): _isetp(w, i, m, w.R(i.ra), w.R(i.rb))
def op_isetp_i(w, i, m): _isetp(w, i, m, w.R(i.ra), u32(i.imm20_sint))


def _p2r(w, i, m, a, mask):       # InstEmitPredicate EmitP2r
    if i.bit(40):
        raise Unsupported(f"P2R.CC {i}")
    shift = i.bits(41, 2) * 8
    mask = (mask & U32(0xFF)) << U32(shift)
    insert = np.zeros(LANES, dtype=U32)
    for k in range(7):
        insert |= np.where(w.p[:, k], U32(1 << (k + shift)), U32(0))
    w.W(i.rd, (a & ~mask) | (insert & mask), m)


def op_p2r_r(w, i, m): _p2r(w, i, m, w.R(i.ra), w.R(i.rb))
def op_p2r_i(w, i, m): _p2r(w, i, m, w.R(i.ra), u32(i.imm20))   # P2rI: the raw Imm20 as mask


def _r2p(w, i, m, value, mask):   # InstEmitMove EmitR2p
    if i.bit(40):
        raise Unsupported(f"R2P.CC {i}")
    shift = i.bits(41, 2) * 8
    for k in range(7):
        sel = (mask >> U32(k)) & U32(1) != 0
        bit = (value >> U32(k + shift)) & U32(1) != 0
        w.WP(k, np.where(sel, bit, w.p[:, k]), m)


def op_r2p_r(w, i, m): _r2p(w, i, m, w.R(i.ra), w.R(i.rb))
def op_r2p_i(w, i, m): _r2p(w, i, m, w.R(i.ra), u32(i.imm20_sint))   # R2pI: Imm20ToSInt


def op_s2r(w, i, m):              # InstEmitMove S2r: SR_LANEID; one warp, so TID.X is the lane and TID.Y/Z are 0
    sreg = i.bits(20, 8)
    if sreg in (0, 33):
        v = w.lane
    elif sreg in (34, 35):
        v = np.zeros(LANES, dtype=U32)
    elif sreg == 32:
        v = w.lane
    else:
        raise Unsupported(f"S2R SR {sreg} {i}")
    w.W(i.rd, v, m)


def op_shfl(w, i, m):             # InstEmitWarp Shfl + HelperFunctionManager GenerateShuffle* (subgroup size 32)
    a = w.R(i.ra)
    b = u32(i.bits(20, 5)) if i.bit(28) else w.R(i.rb)
    c = u32(i.bits(34, 13)) if i.bit(29) else w.R(i.rc)
    lane = w.lane
    clamp = c & U32(0x1F)
    seg = (c >> U32(8)) & U32(0x1F)
    min_t = lane & seg
    max_t = (clamp & ~seg) | min_t
    mode = i.bits(30, 2)
    if mode == 0:     # IDX
        src = (b & ~seg) | min_t
        valid = src <= max_t
    elif mode == 1:   # UP: signed compare (ICompareGreaterOrEqual)
        src = lane - b
        valid = src.view(np.int32) >= min_t.view(np.int32)
    elif mode == 2:   # DOWN
        src = lane + b
        valid = src <= max_t
    else:             # BFLY
        src = lane ^ b
        valid = src <= max_t
    idx = np.where(valid, src, lane).astype(np.int64)
    # A predicated SHFL becomes an if-block in SPIR-V: reading a lane outside it is undefined.
    bad = m & valid & ~m[np.clip(idx, 0, LANES - 1)]
    if bad.any():
        w.warnings.append(f"SHFL at {i} reads inactive lanes {sorted(set(idx[bad].tolist()))}")
    res = np.where(valid, a[np.clip(idx, 0, LANES - 1)], a)
    w.W(i.rd, res, m)
    w.WP(i.bits(48, 3), valid, m)


def _mem_load(w, i, m, mem_of, size, base, offset, what):
    """InstEmitMemory EmitLoad: per word, the word at (address >> 2), sub-words extracted at (address & 3) * 8."""
    if size > 6:
        raise Unsupported(f"{what} size {size} {i}")
    count = {5: 2, 6: 4}.get(size, 1)
    for k in range(count):
        if i.rd + k >= RZ:
            break
        vals = np.zeros(LANES, dtype=U32)
        for lane in np.nonzero(m)[0]:
            mem = mem_of(lane)
            addr = (int(base[lane]) + offset + 4 * k) & 0xFFFFFFFF
            at = (addr >> 2) * 4
            if at + 4 > len(mem):
                raise IndexError(f"{what} lane {lane} address 0x{addr:x} outside {len(mem)} bytes {i}")
            v = int.from_bytes(mem[at:at + 4], "little") >> ((addr & 3) * 8)
            if size == 0:
                v &= 0xFF
            elif size == 1:
                v = sext(v, 8) & 0xFFFFFFFF
            elif size == 2:
                v &= 0xFFFF
            elif size == 3:
                v = sext(v, 16) & 0xFFFFFFFF
            vals[lane] = v
        w.W(i.rd + k, vals, m)


def _mem_store(w, i, m, mem_of, size, base, offset, what, local):
    """EmitStore: 32-bit words at (address >> 2); shared 8/16-bit stores write those bytes at the address, local
    ones insert into the word at (address & 3) * 8. Lanes store in lane order (the last lane wins a conflict)."""
    if size > 6:
        raise Unsupported(f"{what} size {size} {i}")
    count = {5: 2, 6: 4}.get(size, 1)
    for k in range(count):
        reg = i.rd if i.rd + k >= RZ else i.rd + k
        vals = w.R(reg)
        for lane in np.nonzero(m)[0]:
            mem = mem_of(lane)
            addr = (int(base[lane]) + offset + 4 * k) & 0xFFFFFFFF
            v = int(vals[lane])
            if size < 4 and not local:
                n = 1 if size < 2 else 2
                if addr + n > len(mem):
                    raise IndexError(f"{what} lane {lane} address 0x{addr:x} outside {len(mem)} bytes {i}")
                mem[addr:addr + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
                continue
            at = (addr >> 2) * 4
            if at + 4 > len(mem):
                raise IndexError(f"{what} lane {lane} address 0x{addr:x} outside {len(mem)} bytes {i}")
            if size < 4:
                n = 8 if size < 2 else 16
                sh = (addr & 3) * 8
                old = int.from_bytes(mem[at:at + 4], "little")
                fmask = ((1 << n) - 1) << sh
                v = (old & ~fmask & 0xFFFFFFFF) | ((v << sh) & fmask)
            mem[at:at + 4] = (v & 0xFFFFFFFF).to_bytes(4, "little")


def op_lds(w, i, m):
    _mem_load(w, i, m, lambda lane: w.smem, i.bits(48, 3), w.R(i.ra), i.imm24_sint, "LDS")


def op_sts(w, i, m):
    _mem_store(w, i, m, lambda lane: w.smem, i.bits(48, 3), w.R(i.ra), i.imm24_sint, "STS", False)


def op_ldl(w, i, m):
    _mem_load(w, i, m, lambda lane: w.local[lane], i.bits(48, 3), w.R(i.ra), i.imm24_sint, "LDL")


def op_stl(w, i, m):
    _mem_store(w, i, m, lambda lane: w.local[lane], i.bits(48, 3), w.R(i.ra), i.imm24_sint, "STL", True)


def op_f2f_r(w, i, m):            # InstEmitConversion F2fR / EmitF2F
    _no_cc(i, 47, None, "F2F")
    dst, src = i.bits(8, 2), i.bits(10, 2)
    if dst not in (1, 2) or src not in (1, 2):
        raise Unsupported(f"F2F with F64 {i}")
    x = w.halves(i.rb)[1 if i.bit(41) else 0] if src == 1 else as_f32(w.R(i.rb))
    x = abs_neg(x, i.bit(49), i.bit(45))
    rnd = (i.bits(42, 1) << 2) | i.bits(39, 2)   # IntegerRound: 4 round, 5 floor, 6 ceil, 7 trunc
    if src == dst:
        x = {4: np.rint, 5: np.floor, 6: np.ceil, 7: np.trunc}.get(rnd, lambda v: v)(x).astype(F32)
    if i.bit(50):
        x = saturate(x)
    if dst == 2:
        w.W(i.rd, from_f32(x), m)
    else:                         # WriteFP: PackHalf2x16(x, 0)
        zero = np.zeros(LANES, F32)
        w.W(i.rd, pack_half(x, zero), m, halves=(x, zero))


def op_mufu(w, i, m):             # InstEmitMultifunction Mufu: FPAbsNeg of Ra, the function in F32, FPSaturate
    fn = {2: np.exp2, 3: np.log2, 4: lambda v: F32(1) / v, 5: lambda v: F32(1) / np.sqrt(v), 8: np.sqrt}.get(i.bits(20, 4))
    if fn is None:
        raise Unsupported(f"MUFU operation {i.bits(20, 4)} {i}")
    x = abs_neg(as_f32(w.R(i.ra)), i.bit(46), i.bit(48))
    with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
        r = fn(x).astype(F32)
    w.W(i.rd, from_f32(saturate(r) if i.bit(50) else r), m)


def _ffma(w, i, m, a, b, c, neg_b, neg_c, sat):   # EmitFfma (rounding mode and FMZ ignored)
    r = fma_f32(as_f32(a), abs_neg(as_f32(b), False, neg_b), abs_neg(as_f32(c), False, neg_c))
    w.W(i.rd, from_f32(saturate(r) if sat else r), m)


def op_ffma_r(w, i, m):
    _no_cc(i, 47, None, "FFMA")
    _ffma(w, i, m, w.R(i.ra), w.R(i.rb), w.R(i.rc), i.bit(48), i.bit(49), i.bit(50))


def op_ffma_i(w, i, m):
    _no_cc(i, 47, None, "FFMA")
    _ffma(w, i, m, w.R(i.ra), u32(i.imm20_float), w.R(i.rc), i.bit(48), i.bit(49), i.bit(50))


def op_ffma32i(w, i, m):          # Ffma32i: C is the destination register
    _no_cc(i, 52, None, "FFMA32I")
    _ffma(w, i, m, w.R(i.ra), u32(i.imm32), w.R(i.rd), i.bit(56), i.bit(57), i.bit(55))


def _fmul(w, i, m, a, b, scale, neg_b, sat):      # EmitFmul: A times the scale first, then A * B
    a = as_f32(a)
    if scale:
        a = (a * F32({1: 0.5, 2: 0.25, 3: 0.125, 4: 8.0, 5: 4.0, 6: 2.0}.get(scale, 1.0))).astype(F32)
    r = (a * abs_neg(as_f32(b), False, neg_b)).astype(F32)
    w.W(i.rd, from_f32(saturate(r) if sat else r), m)


def op_fmul_r(w, i, m):
    _no_cc(i, 47, None, "FMUL")
    _fmul(w, i, m, w.R(i.ra), w.R(i.rb), i.bits(41, 3), i.bit(48), i.bit(50))


def op_fmul_i(w, i, m):
    _no_cc(i, 47, None, "FMUL")
    _fmul(w, i, m, w.R(i.ra), u32(i.imm20_float), i.bits(41, 3), i.bit(48), i.bit(50))


def op_fmul32i(w, i, m):
    _no_cc(i, 52, None, "FMUL32I")
    _fmul(w, i, m, w.R(i.ra), u32(i.imm32), 0, False, i.bit(55))


def _fadd(w, i, m, a, b, neg_a, neg_b, abs_a, abs_b, sat):   # EmitFadd
    r = (abs_neg(as_f32(a), abs_a, neg_a) + abs_neg(as_f32(b), abs_b, neg_b)).astype(F32)
    w.W(i.rd, from_f32(saturate(r) if sat else r), m)


def op_fadd_r(w, i, m):
    _no_cc(i, 47, None, "FADD")
    _fadd(w, i, m, w.R(i.ra), w.R(i.rb), i.bit(48), i.bit(45), i.bit(46), i.bit(49), i.bit(50))


def op_fadd_i(w, i, m):
    _no_cc(i, 47, None, "FADD")
    _fadd(w, i, m, w.R(i.ra), u32(i.imm20_float), i.bit(48), i.bit(45), i.bit(46), i.bit(49), i.bit(50))


def op_fadd32i(w, i, m):
    _no_cc(i, 52, None, "FADD32I")
    _fadd(w, i, m, w.R(i.ra), u32(i.imm32), i.bit(56), i.bit(53), i.bit(54), i.bit(57), False)


# Half precision: InstEmitHelper GetHalfSrc / GetHalfUnpacked / GetHalfPacked.
def half_src(w: Warp, reg: int, swizzle: int, neg: bool = False, absolute: bool = False):
    """The two F32 values of a packed-half register operand: swizzle 0 (H1_H0), 1 (F32: the register as one F32
    twice), 2 (H0_H0), 3 (H1_H1); then abs, neg. The halves come through Warp.halves (PropagatePack)."""
    if swizzle == 1:
        x = as_f32(w.R(reg))
        pair = (x, x)
    else:
        lo, hi = w.halves(reg)
        pair = {0: (lo, hi), 2: (lo, lo), 3: (hi, hi)}[swizzle]
    return [abs_neg(x, absolute, neg) for x in pair]


def half_imm_pair(h0: int, h1: int):
    """GetHalfSrc(immH0, immH1): each 10-bit field shifted left 6 as an F16."""
    return [np.full(LANES, np.array([(v << 6) & 0xFFFF], dtype=np.uint16).view(F16)[0], dtype=F32) for v in (h0, h1)]


def half_imm32(imm: int):
    return [np.full(LANES, np.array([(imm >> s) & 0xFFFF], dtype=np.uint16).view(F16)[0], dtype=F32) for s in (0, 16)]


def half_pack(w: Warp, ofmt: int, res, rd: int):
    """GetHalfPacked: 0 F16 pair, 1 F32 (lane 0's result), 2 MRG_H0 (high half kept), 3 MRG_H1 (low half kept).
    Returns the register value and the F32 halves PackHalf2x16 rounded (None for the F32 format)."""
    if ofmt == 1:
        return from_f32(res[0]), None
    if ofmt == 0:
        lo, hi = res[0], res[1]
    else:
        old_lo, old_hi = w.halves(rd)   # GetHalfDest: an UnpackHalf2x16 of Rd
        lo, hi = (res[0], old_hi) if ofmt == 2 else (old_lo, res[1])
    return pack_half(lo, hi), (lo, hi)


def _hadd_hmul(w, i, m, a, b, add, ofmt, sat):    # EmitHadd2Hmul2: F32 add/multiply per half, saturate, pack
    res = []
    for k in range(2):
        r = (a[k] + b[k]).astype(F32) if add else (a[k] * b[k]).astype(F32)
        res.append(saturate(r) if sat else r)
    v, halves = half_pack(w, ofmt, res, i.rd)
    w.W(i.rd, v, m, halves=halves)


def _hfma(w, i, m, a, b, c, ofmt, sat):           # EmitHfma2: F32 fused multiply-add per half, saturate, pack
    res = []
    for k in range(2):
        r = fma_f32(a[k], b[k], c[k])
        res.append(saturate(r) if sat else r)
    v, halves = half_pack(w, ofmt, res, i.rd)
    w.W(i.rd, v, m, halves=halves)


def op_hadd2_r(w, i, m):
    a = half_src(w, i.ra, i.bits(47, 2), i.bit(43), i.bit(44))
    b = half_src(w, i.rb, i.bits(28, 2), i.bit(31), i.bit(30))
    _hadd_hmul(w, i, m, a, b, True, i.bits(49, 2), i.bit(32))


def op_hadd2_i(w, i, m):
    a = half_src(w, i.ra, i.bits(47, 2), i.bit(43), i.bit(44))
    b = half_imm_pair(i.bits(20, 10), ((i.w >> 47) & 0x200) | i.bits(30, 9))
    _hadd_hmul(w, i, m, a, b, True, i.bits(49, 2), i.bit(52))


def op_hadd232i(w, i, m):
    a = half_src(w, i.ra, i.bits(53, 2), i.bit(56), False)
    _hadd_hmul(w, i, m, a, half_imm32(i.imm32), True, 0, i.bit(52))


def op_hmul2_r(w, i, m):          # Hmul2R: the NegA bit (31) negates B
    a = half_src(w, i.ra, i.bits(47, 2), False, i.bit(44))
    b = half_src(w, i.rb, i.bits(28, 2), i.bit(31), i.bit(30))
    _hadd_hmul(w, i, m, a, b, False, i.bits(49, 2), i.bit(32))


def op_hmul2_i(w, i, m):
    a = half_src(w, i.ra, i.bits(47, 2), i.bit(43), i.bit(44))
    b = half_imm_pair(i.bits(20, 10), ((i.w >> 47) & 0x200) | i.bits(30, 9))
    _hadd_hmul(w, i, m, a, b, False, i.bits(49, 2), i.bit(52))


def op_hmul232i(w, i, m):
    a = half_src(w, i.ra, i.bits(53, 2))
    _hadd_hmul(w, i, m, a, half_imm32(i.imm32), False, 0, i.bit(52))


def op_hfma2_r(w, i, m):          # Hfma2R: NegA (31) negates B, NegC (30)
    a = half_src(w, i.ra, i.bits(47, 2))
    b = half_src(w, i.rb, i.bits(28, 2), i.bit(31))
    c = half_src(w, i.rc, i.bits(35, 2), i.bit(30))
    _hfma(w, i, m, a, b, c, i.bits(49, 2), i.bit(32))


def op_hfma2_i(w, i, m):
    a = half_src(w, i.ra, i.bits(47, 2))
    b = half_imm_pair(i.bits(20, 10), ((i.w >> 47) & 0x200) | i.bits(30, 9))
    c = half_src(w, i.rc, i.bits(53, 2), i.bit(51))
    _hfma(w, i, m, a, b, c, i.bits(49, 2), i.bit(52))


def op_hfma232i(w, i, m):         # Hfma232i: C is the destination register, H1_H0
    a = half_src(w, i.ra, i.bits(47, 2))
    c = half_src(w, i.rd, 0, i.bit(51))
    _hfma(w, i, m, a, half_imm32(i.imm32), c, 0, False)


# Control flow: handled by the executor (EXIT retires lanes, BRA moves the warp).
def op_exit(w, i, m):
    if i.bits(0, 5) != 15:
        raise Unsupported(f"EXIT with condition code test {i.bits(0, 5)} {i}")
    w.active &= ~m


OP_BRA = "bra"

# The decoder: patterns as the translator's Decoders/InstTable.cs writes them (bit 63 first, x = any). The most specific
# matching pattern wins, as InstTable's XBits ordering does.
TABLE = [
    ("0101000010110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "NOP", op_nop),
    ("111000110000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "EXIT", op_exit),
    ("111000100100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "BRA", OP_BRA),
    ("0101110010011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "MOV", op_mov_r),
    ("0011100x10011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "MOV.I", op_mov_i),
    ("0100110010011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "MOV.C", op_mov_c),
    ("000000010000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "MOV32I", op_mov32i),
    ("0101110000010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "IADD", op_iadd_r),
    ("0011100x00010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "IADD.I", op_iadd_i),
    ("0001110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "IADD32I", op_iadd32i),
    ("010111001100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "IADD3", op_iadd3_r),
    ("0011100x1100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "IADD3.I", op_iadd3_i),
    ("0101110000011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "ISCADD", op_iscadd_r),
    ("0011100x00011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "ISCADD.I", op_iscadd_i),
    ("000101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "ISCADD32I", op_iscadd32i),
    ("0101110001001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "SHL", op_shl_r),
    ("0011100x01001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "SHL.I", op_shl_i),
    ("0101110000101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "SHR", op_shr_r),
    ("0011100x00101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "SHR.I", op_shr_i),
    ("0101110001000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "LOP", op_lop_r),
    ("0011100x01000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "LOP.I", op_lop_i),
    ("000001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "LOP32I", op_lop32i),
    ("0101101111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "LOP3", op_lop3_r),
    ("001111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "LOP3.I", op_lop3_i),
    ("010110111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "PRMT", op_prmt_r),
    ("0011011x1100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "PRMT.I", op_prmt_i),
    ("0101110010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "SEL", op_sel_r),
    ("0011100x10100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "SEL.I", op_sel_i),
    ("010110110110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "ISETP", op_isetp_r),
    ("0011011x0110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "ISETP.I", op_isetp_i),
    ("0101110011101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "P2R", op_p2r_r),
    ("0011100x11101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "P2R.I", op_p2r_i),
    ("0101110011110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "R2P", op_r2p_r),
    ("0011100x11110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "R2P.I", op_r2p_i),
    ("1111000011001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "S2R", op_s2r),
    ("1110111100010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "SHFL", op_shfl),
    ("1110111101001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "LDS", op_lds),
    ("1110111101011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "STS", op_sts),
    ("1110111101000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "LDL", op_ldl),
    ("1110111101010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "STL", op_stl),
    ("0101110010101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "F2F", op_f2f_r),
    ("0101000010000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "MUFU", op_mufu),
    ("010110011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FFMA", op_ffma_r),
    ("0011001x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FFMA.I", op_ffma_i),
    ("000011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FFMA32I", op_ffma32i),
    ("0101110001101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FMUL", op_fmul_r),
    ("0011100x01101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FMUL.I", op_fmul_i),
    ("00011110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FMUL32I", op_fmul32i),
    ("0101110001011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FADD", op_fadd_r),
    ("0011100x01011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FADD.I", op_fadd_i),
    ("000010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "FADD32I", op_fadd32i),
    ("0101110100010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HADD2", op_hadd2_r),
    ("0111101x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HADD2.I", op_hadd2_i),
    ("0010110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HADD2_32I", op_hadd232i),
    ("0101110100001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HMUL2", op_hmul2_r),
    ("0111100x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HMUL2.I", op_hmul2_i),
    ("0010101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HMUL2_32I", op_hmul232i),
    ("0101110100000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HFMA2", op_hfma2_r),
    ("01110xxx0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HFMA2.I", op_hfma2_i),
    ("0010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "HFMA2_32I", op_hfma232i),
]


def _compile_table():
    out = []
    for pattern, name, fn in TABLE:
        assert len(pattern) == 64, name
        mask = value = 0
        for k, ch in enumerate(pattern):
            bit = 63 - k
            if ch != "x":
                mask |= 1 << bit
                value |= (ch == "1") << bit
        out.append((mask, value, bin(mask).count("1"), name, fn))
    return out


_DECODE = _compile_table()


def decode(word: int):
    """(name, handler) of a Maxwell word, by the most specific matching InstTable pattern."""
    best = None
    for mask, value, fixed, name, fn in _DECODE:
        if word & mask == value and (best is None or fixed > best[0]):
            best = (fixed, name, fn)
    if best is None:
        raise UnknownOpcode(f"unknown Maxwell opcode 0x{word:016x}")
    return best[1], best[2]


def slot_address(k: int) -> int:
    """Byte address of instruction k in a compute program as AmpereLowering.Lower lays it out (a scheduling word
    first in every 32 bytes; --lower prints the instruction words only)."""
    return 8 * (1 + k + k // 3)


def run_warp(words, regs_in=None, smem=None, max_steps: int = 200000, **opts) -> Warp:
    """Executes the Maxwell words on one warp and returns its final state. regs_in in any form load_regs takes."""
    words = [w if isinstance(w, int) else int(str(w), 16) for w in words]
    w = Warp(smem=parse_bytes(smem) if smem is not None else None, **opts)
    for lane, regs in _normalised(regs_in or {}).items():
        for reg, val in regs.items():
            kind, n = reg
            if kind == "R":
                w.r[lane, n] = val & 0xFFFFFFFF if n != RZ else 0
            elif kind == "P" and n != PT:
                w.p[lane, n] = bool(val)
    addr_to_index = {slot_address(k): k for k in range(len(words) + 1)}
    # Where control flow joins (branch targets) the translator merges registers in phis, and an UnpackHalf2x16 of a phi is
    # not folded: the PropagatePack state (Warp.sh_ok) is dropped there and at every BRA (conservative: a register no
    # path writes needs no phi).
    joins = set()
    for k, word in enumerate(words):
        if decode(word)[1] is OP_BRA:
            joins.add(addr_to_index.get(slot_address(k) + 8 + Ins(word, k).imm24_sint))
    pc = steps = 0
    with np.errstate(all="ignore"):
        while pc < len(words) and w.active.any():
            steps += 1
            if steps > max_steps:
                raise RuntimeError(f"more than {max_steps} steps (loop?)")
            ins = Ins(words[pc], pc)
            name, fn = decode(ins.w)
            guard = w.P(ins.bits(16, 3), ins.bit(19))
            mask = w.active & guard
            w.unconditional = ins.bits(16, 3) == PT and not ins.bit(19)
            if pc in joins or fn is OP_BRA:
                w.sh_ok[:] = False
            if w.trace:
                print(f"  {pc:4d} {ins.w:016x} {name:<10} lanes {int(mask.sum())}", file=sys.stderr)
            if fn is OP_BRA:
                if ins.bits(0, 5) != 15:
                    raise Unsupported(f"BRA with condition code test {ins.bits(0, 5)} {ins}")
                if mask.any():
                    if (mask != w.active).any():
                        raise Unsupported(f"divergent BRA {ins}")
                    target = slot_address(pc) + 8 + ins.imm24_sint
                    if target not in addr_to_index:
                        raise Unsupported(f"BRA to 0x{target:x}, not an instruction slot {ins}")
                    pc = addr_to_index[target]
                    continue
            else:
                fn(w, ins, mask)
            pc += 1
    return w


def run(words, regs_in=None, smem=None, **opts) -> np.ndarray:
    """Executes the Maxwell words; returns the per-lane register file, a (32, 256) uint32 array (row = lane)."""
    return run_warp(words, regs_in, smem, **opts).r


# ---- vector files and the --lower bridge --------------------------------------------------------------------------

def parse_int(v) -> int:
    """An int, or a hex string (with or without 0x)."""
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, np.integer)):
        return int(v)
    s = str(v).strip().lower()
    return int(s[2:] if s.startswith("0x") else s, 16)


def parse_bytes(v) -> bytes:
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    if isinstance(v, list):
        return bytes(parse_int(x) & 0xFF for x in v)
    s = re.sub(r"\s+", "", str(v))
    return bytes.fromhex(s[2:] if s.lower().startswith("0x") else s)


def parse_reg(name, urbase: int | None = None):
    """("R", n) or ("P", n) for a register name: an int, "R12", "12", "RZ", "UR3" (needs urbase), "P0", "PT"."""
    if isinstance(name, (int, np.integer)):
        return ("R", int(name))
    s = str(name).strip().upper()
    if s in ("RZ", "R255"):
        return ("R", RZ)
    if s == "PT":
        return ("P", PT)
    if s.startswith("UR"):
        if s == "URZ":
            return ("R", RZ)
        if urbase is None:
            raise ValueError(f"{name}: uniform register without a urbase")
        return ("R", urbase + int(s[2:]))
    if s.startswith("P"):
        return ("P", int(s[1:]))
    if s.startswith("R"):
        s = s[1:]
    return ("R", int(s))


def load_regs(spec, urbase: int | None = None) -> dict[int, dict]:
    """Normalises register specs to {lane: {(kind, n): int}}. Accepts {lane: {reg: value}} with lane keys as ints or
    strings (and "*" / "all" for every lane), or a list with one entry per lane; each lane entry a dict {reg: value}
    or a list of values indexed by register number; values ints or hex strings."""
    out: dict[int, dict] = {lane: {} for lane in range(LANES)}
    if isinstance(spec, list):
        items = list(enumerate(spec))
    else:
        items = list(spec.items())
    # "*" first so per-lane entries override it.
    items.sort(key=lambda kv: 0 if str(kv[0]).strip().lower() in ("*", "all") else 1)
    for lane_key, regs in items:
        key = str(lane_key).strip().lower()
        lanes = range(LANES) if key in ("*", "all") else [int(key)]
        if isinstance(regs, list):
            regs = {n: v for n, v in enumerate(regs) if v is not None}
        for lane in lanes:
            for reg, val in (regs or {}).items():
                out[lane][parse_reg(reg, urbase)] = parse_int(val)
    return out


def _normalised(spec) -> dict[int, dict]:
    """load_regs output passes through unchanged (keys (kind, n)); anything else is loaded."""
    if isinstance(spec, dict) and all(isinstance(k, int) for k in spec) and \
            all(isinstance(r, tuple) for regs in spec.values() if isinstance(regs, dict) for r in regs):
        return spec
    return load_regs(spec)


def lower(sm86, exe: Path = DEFAULT_XLATE):
    """Runs nx2navi_xlate --lower on SM86 instructions (a list of [w0, w1, w2, w3] or one such list); returns
    (Maxwell words, urbase, local bytes)."""
    if sm86 and not isinstance(sm86[0], (list, tuple)):
        sm86 = [sm86]
    text = "".join(" ".join(f"{parse_int(x) & 0xFFFFFFFF:08x}" for x in ins) + "\n" for ins in sm86)
    fd, path = tempfile.mkstemp(suffix=".sm86.txt")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        p = subprocess.run([str(exe), "--lower", path], capture_output=True, text=True)
    finally:
        os.unlink(path)
    if p.returncode != 0:
        raise RuntimeError(f"--lower failed: {p.stderr.strip()}")
    words, urbase, local = [], None, 0
    for line in p.stdout.splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "urbase":
            urbase = int(f[1])
        elif f[0] == "local":
            local = int(f[1])
        else:
            words.append(int(f[0], 16))
    return words, urbase, local


def _fmt(v: int) -> str:
    """A register value as hex plus its F32 and half-pair readings."""
    f = np.array([v], dtype=U32).view(F32)[0]
    h = np.array([v & 0xFFFF, v >> 16], dtype=np.uint16).view(F16)
    return f"{v:08x} (f32 {f:.6g}, h {float(h[0]):.5g}/{float(h[1]):.5g})"


# ---- comparing results ----------------------------------------------------------------------------------------------
# A float result is right when its bits are, except that any NaN matches any NaN: SPIR-V leaves the payload and sign
# of a NaN to the host (Vulkan spec, "Precision and Operation of SPIR-V Instructions"), and this emulator's numpy
# arithmetic picks its own, while SM86 hardware writes 0x7fff for every F16 NaN result. Whether a value is NaN is the
# lowering's doing; which NaN is not. Moves (LDSM, MOVM) and integer results compare bit for bit.

def result_kind(sm86) -> str:
    """How the destination registers of an SM86 instruction compare: "f16x2" (two F16 values), "f32" or "bits".
    sm86 is [w0, w1, w2, w3] or a list of them (the last one decides)."""
    ins = sm86[-1] if isinstance(sm86[0], (list, tuple)) else sm86
    w0, w2 = parse_int(ins[0]), parse_int(ins[2])
    op = w0 & 0xFFF
    if op == 0x23C:                              # HMMA: .F32 (bit 76) or .F16 results
        return "f32" if (w2 >> 12) & 1 else "f16x2"
    if (op & 0x1FF) in (0x030, 0x031, 0x032):    # HADD2 / HFMA2 / HMUL2, F32 results with bit 78
        return "f32" if (w2 >> 14) & 1 else "f16x2"
    return "bits"


def _nan16(h: int) -> bool:
    return (h & 0x7FFF) > 0x7C00


def _nan32(v: int) -> bool:
    return (v & 0x7FFFFFFF) > 0x7F800000


def same_value(got: int, want: int, kind: str) -> bool:
    """got matches want: equal bits, or for float kinds NaN where want is NaN (see above)."""
    got, want = got & 0xFFFFFFFF, want & 0xFFFFFFFF
    if got == want:
        return True
    if kind == "f32":
        return _nan32(got) and _nan32(want)
    if kind == "f16x2":
        return all(g == w or (_nan16(g) and _nan16(w))
                   for g, w in ((got & 0xFFFF, want & 0xFFFF), (got >> 16, want >> 16)))
    return False


def ulps(got: int, want: int, kind: str) -> int:
    """Distance in units in the last place (the larger of the two halves for "f16x2"); 0 for matching NaNs, a huge
    number for NaN against a number. +0 and -0 are 0 apart (same_value still tells them apart)."""
    def order(v, sign_bit, nan):
        if nan(v):
            return None
        mag = v & (sign_bit - 1)
        return -mag if v & sign_bit else mag
    def one(g, w, sign_bit, nan):
        og, ow = order(g, sign_bit, nan), order(w, sign_bit, nan)
        if og is None or ow is None:
            return 0 if og is None and ow is None else 1 << 32
        return abs(og - ow)
    got, want = got & 0xFFFFFFFF, want & 0xFFFFFFFF
    if kind == "f32":
        return one(got, want, 1 << 31, _nan32)
    if kind == "f16x2":
        return max(one(got & 0xFFFF, want & 0xFFFF, 1 << 15, _nan16), one(got >> 16, want >> 16, 1 << 15, _nan16))
    return 0 if got == want else 1 << 32


def run_case(case: dict, exe: Path, prmt: str, trace: bool = False, fill: int = 0, exact: bool = False):
    """Lowers, runs and compares one vector case; returns (ok, lines of report). Float results compare with
    same_value (a NaN matches a NaN) unless exact."""
    kind = "bits" if exact else result_kind(case["sm86"])
    words, urbase, _ = lower(case["sm86"], exe)
    regs_in = load_regs(case.get("regs_in", {}), urbase)
    expected = load_regs(case.get("regs_out", {}), urbase)
    # LDSM cases (work/tensor-gt/README.md): "lane_addresses" hold Ra per lane when regs_in does not, relative to
    # "smem", which sits at shared address "smem_base". Shared memory is padded to 64 KiB so stray reads stay in range.
    smem = None
    if case.get("smem") is not None:
        base = case.get("smem_base", 0)
        base = base if isinstance(base, int) else parse_int(base)
        data = parse_bytes(case["smem"])
        buf = bytearray(max(0x10000, base + len(data)))
        buf[base:base + len(data)] = data
        smem = bytes(buf)
    sm86 = case["sm86"] if isinstance(case["sm86"][0], (list, tuple)) else [case["sm86"]]
    if case.get("lane_addresses") is not None and len(sm86) == 1:
        ra = (parse_int(sm86[0][0]) >> 24) & 0xFF
        base = case.get("smem_base", 0)
        base = base if isinstance(base, int) else parse_int(base)
        for lane, a in enumerate(case["lane_addresses"]):
            regs_in[lane].setdefault(("R", ra), (parse_int(a) + base) & 0xFFFFFFFF)
    warp = run_warp(words, regs_in, smem, prmt=prmt, trace=trace, fill=fill)
    diffs = []
    for lane in range(LANES):
        for (reg_kind, n), want in sorted(expected[lane].items()):
            if reg_kind == "R":
                got = int(warp.r[lane, n])
                if not same_value(got, want, kind):
                    diffs.append((lane, reg_kind, n, got, want))
            elif int(warp.p[lane, n]) != int(bool(want)):
                diffs.append((lane, reg_kind, n, int(warp.p[lane, n]), want))
    if "smem_out" in case:
        want_mem = parse_bytes(case["smem_out"])
        if bytes(warp.smem[:len(want_mem)]) != want_mem:
            diffs.append((-1, "smem", 0, 0, 0))
    report = [f"  {len(words)} Maxwell words, urbase {urbase}"]
    report += [f"  warning: {m}" for m in dict.fromkeys(warp.warnings)]
    if not diffs:
        return True, report
    lanes = sorted({d[0] for d in diffs})
    report.append(f"  {len(diffs)} differing values in lanes {lanes[:16]}{' ...' if len(lanes) > 16 else ''}")
    if kind != "bits":
        worst = max(ulps(got, want, kind) for _, reg_kind, _, got, want in diffs if reg_kind == "R")
        report.append(f"  largest difference: {'NaN against a number' if worst >= 1 << 32 else f'{worst} ulp'} ({kind})")
    for lane, reg_kind, n, got, want in diffs[:12]:
        if reg_kind == "smem":
            report.append("  shared memory differs from smem_out")
        elif reg_kind == "P":
            report.append(f"  lane {lane:2d} P{n}: got {got} want {int(bool(want))}")
        else:
            report.append(f"  lane {lane:2d} R{n}: got {_fmt(got)} want {_fmt(want & 0xFFFFFFFF)}")
    return False, report


def load_cases(path: Path) -> list[dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(data, dict):
        data = data.get("cases", data.get("vectors", list(data.values()) if all(isinstance(v, dict) for v in data.values()) else []))
    return data


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Lower SM86 vector cases with nx2navi_xlate --lower, run them on the "
                                             "Maxwell warp emulator and compare with the captured results.")
    ap.add_argument("vectors", type=Path, help="JSON list of cases: name, sm86 [w0..w3], regs_in, smem?, regs_out")
    ap.add_argument("--xlate", type=Path, default=DEFAULT_XLATE, help="nx2navi_xlate executable")
    ap.add_argument("--prmt", choices=("ryujinx", "maxwell"), default="ryujinx",
                    help="PRMT as the translator runs it (no-op) or as Maxwell hardware does")
    ap.add_argument("--fill", default="0", help="initial value (hex) of registers regs_in does not set")
    ap.add_argument("--case", action="append", help="run only cases whose name contains this (repeatable)")
    ap.add_argument("--trace", action="store_true", help="print every executed Maxwell instruction")
    ap.add_argument("--exact", action="store_true", help="compare float results bit for bit (NaN payloads too)")
    args = ap.parse_args(argv)
    cases = load_cases(args.vectors)
    passed = failed = 0
    for idx, case in enumerate(cases):
        name = case.get("name", f"case{idx}")
        if args.case and not any(c in name for c in args.case):
            continue
        try:
            ok, report = run_case(case, args.xlate, args.prmt, args.trace, parse_int(args.fill), args.exact)
        except Exception as e:   # one broken case does not stop the run
            ok, report = False, [f"  error: {type(e).__name__}: {e}"]
        print(f"{'PASS' if ok else 'FAIL'} {name}")
        if not ok or any("warning" in r for r in report):
            print("\n".join(report))
        passed += ok
        failed += not ok
    print(f"{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
