"""Checks the lowered SM86 tensor instructions against results captured on an NVIDIA GPU, in one command.

tools/xlate/AmpereLowering.cs rewrites the SM86 instructions of the game's neural-network passes as Maxwell code:
Lowerer.Hmma (HMMA.1688 / HMMA.16816, .F16 and .F32), Lowerer.Ldsm (LDSM.16.M88 / MT88 .1/.2/.4), Lowerer.Movm
(MOVM.16.MT88) and Lowerer.Half (HFMA2, HFMA2.RELU). This script lowers every case of the vector file with
`nx2navi_xlate --lower`, runs the Maxwell words on tests/maxwell_warp_emu.py (which executes them as the shader
translator does) and compares with the registers the GPU wrote. Exit status 1 on any failure. The vector file names
the GPU that produced it ("device"). So far that is an RTX 5090 running the compute_86 PTX through the driver's JIT,
as no SM86 part was at hand: the results follow the PTX semantics but need not show SM86's internal precision.

  C:/Users/PC/AppData/Local/Programs/Python/Python312/python tests/tensor_lowering_check.py [vectors.json]
      [--xlate EXE] [--case NAME] [-v]

Defaults: vectors.json is <workspace>/work/tensor-gt/vectors.json (<workspace>/work/tensor-gt/gen_vectors.py writes
it: each case is one SM86 instruction in inline PTX, run on one warp), the translator
<workspace>/build/xlate/nx2navi_xlate.exe (python tools/build_xlate.py builds it); <workspace> is the directory two
levels above this repository. numpy is needed.

Every case runs several times and must pass each run:
  * with the registers the vectors do not set first 0, then 0x7fc0dead, so a read of a register the lowering never
    wrote shows;
  * then under @P0 (the guard field of the SM86 word set to P0) with P0 false in every third lane: lanes with P0 get
    the GPU's result, the others keep their old destination values. The other lanes still supply their fragments,
    rows and addresses, which works only if the shuffles run in every lane and just the destination writes take the
    guard, the rule AmpereLowering.cs follows.
Each run checks:
  * destination registers bit for bit, except that a NaN matches any NaN in a float result (maxwell_warp_emu
    same_value: SPIR-V leaves NaN payloads to the host, the GPU writes 0x7fff). HMMA gets no tolerance: the GPU rounds
    every element once (f16 or f32 of the exact A*B+C), and so does the lowering for these values, whose sums are
    exact in F32; the largest difference in ulp is reported;
  * every other register R0-R236 (guest and uniform registers; R237 up is the lowering's own) and P0-P6 keep their
    values, shared memory is untouched, and the emulator has nothing to report (a shuffle reading a lane its guard
    left out, a PRMT the translator drops);
  * urbase, the register the lowering mapped UR0 to, is the highest register the instruction names plus two, rounded
    down to even: AmpereLowering HighestGpr counted every register of the instruction's fragments and quads.
Read off the lowered words (both lowerings): every write to a guest register carries the instruction's guard and no
SHFL is guarded.
Derived cases, beyond the hardware vectors, for forms the game uses: reference models of HFMA2(.RELU) and LDSM, each
first checked against every hardware case of its instruction, give the expected values of HFMA2.RELU with an
immediate B (opcode 0x831, as in the game's HFMA2.RELU Rd, Ra, 1, 1, Rc) and LDSM .M88.4 / .MT88.4 with an offset.
MUFU.F16 (LG2 and EX2 of either half, with abs and neg, on the HFMA2 case's inputs) is checked against a model of
the lowering (F32 function of the widened half, rounded back to a half): no hardware vectors, as PTX has no lg2.f16;
the instruction forms were confirmed with nvdisasm.
"""

from __future__ import annotations

import argparse
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import maxwell_warp_emu as emu  # noqa: E402

LANES = 32
RZ = 255
GUEST_REGS = 237                  # R0-R236: guest and uniform registers; AmpereLowering.cs uses R237 up itself
FILLS = (0x00000000, 0x7FC0DEAD)
EXIT = 0xE30000000007000F         # Maxwell EXIT: runs no code, gives the emulator's initial state
DEFAULT_VECTORS = emu.ROOT.parent.parent / "work" / "tensor-gt" / "vectors.json"


def guard_lanes() -> np.ndarray:
    """P0 for the guarded run: false in lanes 1, 4, 7, ... so every quad and row group mixes both."""
    return np.arange(LANES) % 3 != 1


# ---- the instructions of the vector file ----------------------------------------------------------------------

def words_of(sm86) -> list[int]:
    return [emu.parse_int(x) & 0xFFFFFFFF for x in sm86]


def operands(sm86) -> tuple[list[int], list[int]]:
    """(destination registers, source registers) one SM86 instruction names, from its fields (work/tensor-gt/README.md
    "Encoding bits"; the same fields AmpereLowering.cs decodes)."""
    w0, w1, w2, _ = words_of(sm86)
    op = w0 & 0xFFF
    rd, ra, rb, rc = (w0 >> 16) & 0xFF, (w0 >> 24) & 0xFF, w1 & 0xFF, w2 & 0xFF

    def regs(base, n):
        return [] if base == RZ else list(range(base, base + n))
    if op == 0x23C:                                     # HMMA: shape bit 75, .F32 bit 76
        k16, f32 = (w2 >> 11) & 1, (w2 >> 12) & 1
        cd = 4 if f32 else 2
        return regs(rd, cd), regs(ra, 4 if k16 else 2) + regs(rb, 2 if k16 else 1) + regs(rc, cd)
    if op == 0x83B:                                     # LDSM: count 1 << bits 72-73
        return regs(rd, 1 << ((w2 >> 8) & 3)), regs(ra, 1)
    if op == 0x23A:                                     # MOVM
        return regs(rd, 1), regs(ra, 1)
    if op in (0x231, 0x831):                            # HFMA2: B a register (0x231) or an immediate (0x831)
        return regs(rd, 1), regs(ra, 1) + (regs(rb, 1) if op == 0x231 else []) + regs(rc, 1)
    if op == 0x308:                                     # MUFU (register form): the operand is B
        return regs(rd, 1), regs(rb, 1)
    raise ValueError(f"no operand table for SM86 opcode 0x{op:03x}")


def expected_urbase(sm86) -> int:
    dests, srcs = operands(sm86)
    return (max(dests + srcs + [0]) + 2) & ~1


def shared_memory(case: dict) -> bytes | None:
    """The case's shared memory as maxwell_warp_emu.run_case places it: smem at smem_base, 64 KiB at least."""
    if case.get("smem") is None:
        return None
    base = emu.parse_int(case.get("smem_base", 0))
    data = emu.parse_bytes(case["smem"])
    buf = bytearray(max(0x10000, base + len(data)))
    buf[base:base + len(data)] = data
    return bytes(buf)


def consistency(case: dict) -> list[str]:
    """Problems of the case itself: regs_out not exactly the destinations, LDSM addresses that disagree."""
    problems = []
    dests, _ = operands(case["sm86"])
    for lane, regs in emu.load_regs(case["regs_out"]).items():
        if sorted(n for kind, n in regs if kind == "R") != sorted(dests):
            problems.append(f"lane {lane}: regs_out has {sorted(n for _, n in regs)}, the instruction writes {dests}")
            break
    if case.get("lane_addresses") is not None:
        ra = (words_of(case["sm86"])[0] >> 24) & 0xFF
        base = emu.parse_int(case.get("smem_base", 0))
        regs_in = emu.load_regs(case["regs_in"])
        for lane, a in enumerate(case["lane_addresses"]):
            given = regs_in[lane].get(("R", ra))
            if given is not None and given != (emu.parse_int(a) + base) & 0xFFFFFFFF:
                problems.append(f"lane {lane}: R{ra} = {given:#x} but lane_addresses says {emu.parse_int(a) + base:#x}")
                break
    return problems


# ---- running and comparing --------------------------------------------------------------------------------------

class Outcome:
    def __init__(self):
        self.problems: list[str] = []
        self.worst_ulp = 0
        self.nan_matches = 0
        self.runs = 0


def check_run(out: Outcome, label: str, words, case: dict, fill: int, guard: np.ndarray | None, verbose: bool):
    """One run of the lowered words; appends what is wrong to out.problems."""
    sm86 = case["sm86"]
    dests, _ = operands(sm86)
    kind = emu.result_kind(sm86)
    regs_in = emu.load_regs(case["regs_in"])
    if case.get("lane_addresses") is not None:
        ra = (words_of(sm86)[0] >> 24) & 0xFF
        base = emu.parse_int(case.get("smem_base", 0))
        for lane, a in enumerate(case["lane_addresses"]):
            regs_in[lane].setdefault(("R", ra), (emu.parse_int(a) + base) & 0xFFFFFFFF)
    if guard is not None:
        for lane in range(LANES):
            regs_in[lane][("P", 0)] = bool(guard[lane])
    expected = emu.load_regs(case["regs_out"])
    smem = shared_memory(case)
    start = emu.run_warp([EXIT], regs_in, smem, fill=fill)
    warp = emu.run_warp(words, regs_in, smem, fill=fill)
    out.runs += 1
    where = f"{label}, fill {fill:#010x}"
    bad = []
    for lane in range(LANES):
        on = guard is None or guard[lane]
        for n in dests:
            got = int(warp.r[lane, n])
            want = expected[lane][("R", n)] if on else int(start.r[lane, n])
            k = kind if on else "bits"
            if k != "bits":
                out.worst_ulp = max(out.worst_ulp, emu.ulps(got, want, k))
            if not emu.same_value(got, want, k):
                bad.append(f"lane {lane:2d} R{n}: got {emu._fmt(got)} want {emu._fmt(want & 0xFFFFFFFF)}"
                           + ("" if on else " (guard false: old value)"))
            elif got != want:
                out.nan_matches += 1
    if bad:
        out.problems.append(f"{where}: {len(bad)} wrong destination values")
        out.problems += ["    " + b for b in bad[:12 if verbose else 4]]
    keep = [n for n in range(GUEST_REGS) if n not in dests]
    lanes, cols = np.nonzero(warp.r[:, keep] != start.r[:, keep])
    if len(lanes):
        regs = sorted({keep[c] for c in cols})
        out.problems.append(f"{where}: registers outside the destination changed: R{regs[:8]} in lanes {sorted(set(lanes.tolist()))[:8]}")
    if (warp.p[:, :7] != start.p[:, :7]).any():
        out.problems.append(f"{where}: guest predicates P0-P6 changed")
    if bytes(warp.smem) != bytes(start.smem):
        out.problems.append(f"{where}: shared memory changed")
    for m in dict.fromkeys(warp.warnings):
        out.problems.append(f"{where}: emulator: {m}")


# Maxwell instructions of maxwell_warp_emu.TABLE without a destination register in bits 0-7.
NO_REGISTER_RESULT = {"EXIT", "BRA", "NOP", "STS", "STL", "ISETP", "ISETP.I", "R2P", "R2P.I"}


def guard_rule_problems(words, guard: int, label: str) -> list[str]:
    """AmpereLowering.cs's guard rule read off the lowered words: a write to a guest register (below R237) carries the
    SM86 instruction's guard (Maxwell guard field `guard`), a shuffle runs unguarded, in every lane."""
    problems = []
    for k, w in enumerate(words):
        name, _ = emu.decode(w)
        g = (w >> 16) & 0xF
        if name == "SHFL" and g != 7:
            problems.append(f"{label}: word {k} {w:016x}: SHFL under a guard")
        if name not in NO_REGISTER_RESULT and (w & 0xFF) < GUEST_REGS and g != guard:
            problems.append(f"{label}: word {k} {w:016x}: {name} writes guest register R{w & 0xFF} without the instruction's guard")
    return problems


def check_case(case: dict, exe: Path, verbose: bool = False) -> Outcome:
    out = Outcome()
    out.problems += [f"vector file: {p}" for p in consistency(case)]
    try:
        words, urbase, _ = emu.lower(case["sm86"], exe)
    except RuntimeError as e:
        out.problems.append(str(e))
        return out
    if urbase != expected_urbase(case["sm86"]):
        out.problems.append(f"urbase {urbase}, expected {expected_urbase(case['sm86'])}: HighestGpr misses a register range")
    out.problems += guard_rule_problems(words, (words_of(case["sm86"])[0] >> 12) & 0xF, "lowered words")
    for fill in FILLS:
        check_run(out, "all lanes", words, case, fill, None, verbose)
    # The same instruction under @P0: SM86 guard bits 12-15 = 0.
    w = words_of(case["sm86"])
    guarded = dict(case, sm86=[w[0] & ~0xF000] + w[1:])
    gwords, _, _ = emu.lower(guarded["sm86"], exe)
    out.problems += guard_rule_problems(gwords, 0, "lowered words under @P0")
    check_run(out, "@P0", gwords, guarded, FILLS[1], guard_lanes(), verbose)
    return out


# ---- reference models for the derived cases ------------------------------------------------------------------------

def half_value(h: int):
    """An F16 bit pattern as ("nan"), ("inf", sign) or ("num", Fraction, sign)."""
    sign = h >> 15
    e, m = (h >> 10) & 0x1F, h & 0x3FF
    if e == 0x1F:
        return ("nan",) if m else ("inf", sign)
    v = Fraction(m, 1 << 24) if e == 0 else Fraction(0x400 | m, 1 << 25) * (Fraction(2) ** e)
    return ("num", -v if sign else v, sign)


def round_half(x: Fraction) -> int:
    """x (not zero) rounded to F16, to nearest even; beyond the range infinity."""
    sign = 0x8000 if x < 0 else 0
    m = abs(x)
    e = m.numerator.bit_length() - m.denominator.bit_length()
    if Fraction(2) ** e > m:
        e -= 1
    elif Fraction(2) ** (e + 1) <= m:
        e += 1
    quantum = Fraction(2) ** (max(e, -14) - 10)
    q = m / quantum
    n = q.numerator // q.denominator
    rest = q - n
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and n & 1):
        n += 1
    value = n * quantum
    if value >= 65536:
        return sign | 0x7C00
    return sign | int(np.array([float(value)], dtype=np.float64).astype(np.float16).view(np.uint16)[0])


def fma_half(a: int, b: int, c: int) -> int:
    """F16 a * b + c rounded once (IEEE 754 fusedMultiplyAdd, round to nearest even); a NaN result is 0x7fff."""
    va, vb, vc = half_value(a), half_value(b), half_value(c)
    if "nan" in (va[0], vb[0], vc[0]):
        return 0x7FFF
    zero = lambda v: v[0] == "num" and v[1] == 0
    if (va[0] == "inf" and zero(vb)) or (vb[0] == "inf" and zero(va)):
        return 0x7FFF
    psign = (a >> 15) ^ (b >> 15)
    if va[0] == "inf" or vb[0] == "inf":
        if vc[0] == "inf" and vc[1] != psign:
            return 0x7FFF
        return 0x7C00 | psign << 15
    if vc[0] == "inf":
        return 0x7C00 | vc[1] << 15
    p = va[1] * vb[1]
    s = p + vc[1]
    if s == 0:   # exact zero: -0 only when both terms are -0
        return 0x8000 if (p == 0 and psign and vc[2]) else 0
    return round_half(s)


def hfma2_model(a: int, b: int, c: int, relu: bool) -> int:
    """HFMA2 (.RELU) on both halves (H1_H0 operands): RELU makes negative numbers and -0 +0, keeps NaN as 0x7fff."""
    res = 0
    for shift in (0, 16):
        h = fma_half((a >> shift) & 0xFFFF, (b >> shift) & 0xFFFF, (c >> shift) & 0xFFFF)
        if relu and h != 0x7FFF and h & 0x8000:
            h = 0
        res |= h << shift
    return res


def hfma2_operands(case: dict, lane: int, b_imm: int | None = None):
    w0, w1, w2, _ = words_of(case["sm86"])
    regs = emu.load_regs(case["regs_in"])[lane]
    get = lambda n: 0 if n == RZ else regs[("R", n)]
    b = b_imm if b_imm is not None else get(w1 & 0xFF)
    return get((w0 >> 24) & 0xFF), b, get(w2 & 0xFF), bool((w2 >> 15) & 1)


def ldsm_model(smem: bytes, addresses: list[int], count: int, transpose: bool, offset: int) -> list[list[int]]:
    """LDSM per work/tensor-gt/README.md "Fragment layouts": out[lane][m] is lane's value of Rd + m."""
    out = [[0] * count for _ in range(LANES)]
    rd16 = lambda at: int.from_bytes(smem[at:at + 2], "little")
    for m in range(count):
        rows = [addresses[8 * m + r] + offset for r in range(8)]
        for t in range(LANES):
            if transpose:
                out[t][m] = rd16(rows[2 * (t % 4)] + 2 * (t // 4)) | rd16(rows[2 * (t % 4) + 1] + 2 * (t // 4)) << 16
            else:
                at = rows[t // 4] + 4 * (t % 4)
                out[t][m] = int.from_bytes(smem[at:at + 4], "little")
    return out


def ldsm_fields(sm86):
    w0, w1, w2, _ = words_of(sm86)
    return (w0 >> 16) & 0xFF, (w0 >> 24) & 0xFF, 1 << ((w2 >> 8) & 3), bool((w2 >> 14) & 1), (w1 >> 8) & 0xFFFFFF


def model_problems(cases: list[dict]) -> dict[str, list[str]]:
    """Each model against every hardware case of its instruction (exact bits: the models write 0x7fff for NaN)."""
    found = {}
    for case in cases:
        w0 = words_of(case["sm86"])[0]
        op = w0 & 0xFFF
        expected = emu.load_regs(case["regs_out"])
        bad = []
        if op == 0x231:
            rd = (w0 >> 16) & 0xFF
            for lane in range(LANES):
                want = expected[lane][("R", rd)]
                got = hfma2_model(*hfma2_operands(case, lane))
                if got != want:
                    bad.append(f"lane {lane}: model {got:08x}, GPU {want:08x}")
        elif op == 0x83B:
            rd, ra, count, transpose, offset = ldsm_fields(case["sm86"])
            regs = emu.load_regs(case["regs_in"])
            model = ldsm_model(shared_memory(case), [regs[t][("R", ra)] for t in range(LANES)], count, transpose, offset)
            for t in range(LANES):
                for m in range(count):
                    if model[t][m] != expected[t][("R", rd + m)]:
                        bad.append(f"lane {t} R{rd + m}: model {model[t][m]:08x}, GPU {expected[t][('R', rd + m)]:08x}")
        else:
            continue
        found[case["name"]] = bad
    return found


def derived_cases(cases: list[dict]) -> list[dict]:
    by_name = {c["name"]: c for c in cases}
    out = []
    relu = by_name.get("hfma2_relu")
    if relu is not None:
        w0, _, w2, w3 = words_of(relu["sm86"])
        rb = words_of(relu["sm86"])[1] & 0xFF
        for imm in (0x3C003C00, 0x4000BC00):   # (1, 1) as the game's networks use it; (-1, 2)
            regs_in = [{k: v for k, v in lane.items() if k != f"R{rb}"} for lane in relu["regs_in"]]
            sm86 = [(w0 & ~0xFFF) | 0x831, imm, w2, w3]
            rd = (w0 >> 16) & 0xFF
            regs_out = []
            for lane in range(LANES):
                a, b, c, r = hfma2_operands(relu, lane, imm)
                regs_out.append({f"R{rd}": f"{hfma2_model(a, b, c, r):08x}"})
            out.append({"name": f"derived_hfma2_relu_imm_{imm:08x}", "sm86": [f"{x:08x}" for x in sm86],
                        "regs_in": regs_in, "regs_out": regs_out})
    src = by_name.get("ldsm_m88_x4")
    if src is not None:
        w0, _, w2, w3 = words_of(src["sm86"])
        regs = emu.load_regs(src["regs_in"])
        rd, ra = (w0 >> 16) & 0xFF, (w0 >> 24) & 0xFF
        addresses = [regs[t][("R", ra)] for t in range(LANES)]
        for transpose in (False, True):
            sm86 = [w0, 0x40 << 8, (w2 & ~0x4000) | (0x4000 if transpose else 0), w3]
            model = ldsm_model(shared_memory(src), addresses, 4, transpose, 0x40)
            regs_out = [{f"R{rd + m}": f"{model[t][m]:08x}" for m in range(4)} for t in range(LANES)]
            out.append(dict(src, name=f"derived_ldsm_{'mt88' if transpose else 'm88'}_x4_off40",
                            sm86=[f"{x:08x}" for x in sm86], regs_out=regs_out))
    if relu is not None:   # MUFU.F16 on the halves of the HFMA2 case's A register, as the upscaler's colour decode
        w0 = words_of(relu["sm86"])[0]
        rd, rs = (w0 >> 16) & 0xFF, (w0 >> 24) & 0xFF
        regs = emu.load_regs(relu["regs_in"])
        for name, operation, high, absolute, negate in (("lg2_abs_h1", 3, True, True, False), ("lg2_abs_h0", 3, False, True, False),
                                                        ("ex2_h1", 2, True, False, False), ("ex2_neg_h0", 2, False, False, True)):
            sm86 = [(rd << 16) | (7 << 12) | 0x308, rs | (high << 28) | (absolute << 30) | (negate << 31),
                    (operation << 10) | (1 << 9), 0x000FE200]
            regs_out = [{f"R{rd}": f"{mufu_f16_model(regs[t][('R', rs)], operation, high, absolute, negate):08x}"}
                        for t in range(LANES)]
            out.append({"name": f"derived_mufu_f16_{name}", "sm86": [f"{x:08x}" for x in sm86],
                        "regs_in": relu["regs_in"], "regs_out": regs_out})
    return out


def mufu_f16_model(reg: int, operation: int, high: bool, absolute: bool, negate: bool) -> int:
    """MUFU.F16 as AmpereLowering.cs Mufu lowers it: the half widened to F32 with abs and neg, the function in F32
    (EX2 2, LG2 3), rounded to a half in the low 16 bits, the high 16 bits clear."""
    x = np.array([(reg >> 16) & 0xFFFF if high else reg & 0xFFFF], dtype=np.uint16).view(np.float16).astype(np.float32)
    x = np.negative(np.abs(x) if absolute else x) if negate else (np.abs(x) if absolute else x)
    with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
        y = (np.exp2 if operation == 2 else np.log2)(x).astype(np.float32)
    return int(y.astype(np.float16).view(np.uint16)[0])


# ---- driver -------------------------------------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("vectors", nargs="?", type=Path, default=DEFAULT_VECTORS, help=f"vector file (default {DEFAULT_VECTORS})")
    ap.add_argument("--xlate", type=Path, default=emu.DEFAULT_XLATE, help="nx2navi_xlate executable")
    ap.add_argument("--case", action="append", help="only cases whose name contains this (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true", help="more differing values per failure")
    args = ap.parse_args(argv)
    if not args.vectors.exists():
        print(f"no vector file {args.vectors}")
        return 1
    if not args.xlate.exists():
        print(f"no translator {args.xlate} (python tools/build_xlate.py)")
        return 1
    cases = emu.load_cases(args.vectors)
    failed = 0

    models = model_problems(cases)
    for name, bad in models.items():
        if bad:
            failed += 1
            print(f"FAIL model vs GPU on {name}: " + "; ".join(bad[:4]))
    model_ok = not any(models.values())
    print(f"reference models (HFMA2, LDSM) agree with the GPU on {sum(1 for b in models.values() if not b)}/{len(models)} cases")

    todo = cases + (derived_cases(cases) if model_ok else [])
    passed = 0
    for case in todo:
        name = case["name"]
        if args.case and not any(c in name for c in args.case):
            continue
        try:
            out = check_case(case, args.xlate, args.verbose)
        except Exception as e:   # one broken case does not stop the run
            out = Outcome()
            out.problems.append(f"error: {type(e).__name__}: {e}")
        kind = emu.result_kind(case["sm86"])
        notes = []
        if kind != "bits":
            notes.append("largest difference " + ("NaN against a number" if out.worst_ulp >= 1 << 32 else f"{out.worst_ulp} ulp"))
        if out.nan_matches:
            notes.append(f"{out.nan_matches} NaN values with another NaN encoding than the GPU's")
        ok = not out.problems
        passed += ok
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'} {name} ({out.runs} runs{', ' + ', '.join(notes) if notes else ''})")
        for p in out.problems:
            print("  " + p)
    print(f"{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
