// PocketPenguin - FPA floating-point emulation. Values are kept as host doubles (FPA extended precision
// is wider, but every program we care about was compiled for double or single precision).
#include "fpa.h"
#include "linux.h"
#include <cmath>
#include <cstring>
#include <climits>

namespace gp2x {

static const double kConst[8] = {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 0.5, 10.0};

static inline u32 bit(u32 v, int n) { return (v >> n) & 1; }

// ---- memory formats ----
static double from_single(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static u32 to_single(double d) { float f = (float)d; u32 w; memcpy(&w, &f, 4); return w; }

// FPA stores doubles with the most significant word first.
static double from_double(u32 hi, u32 lo) { u64 b = ((u64)hi << 32) | lo; double d; memcpy(&d, &b, 8); return d; }
static void to_double(double d, u32* hi, u32* lo) { u64 b; memcpy(&b, &d, 8); *hi = (u32)(b >> 32); *lo = (u32)b; }

// Extended: word0 = sign<<31 | exponent (15 bits), word1:word2 = 64-bit mantissa with explicit integer bit.
static void to_ext(double d, u32 w[3]) {
    u32 sign = std::signbit(d) ? 0x80000000u : 0;
    double a = std::fabs(d);
    if (a == 0) { w[0] = sign; w[1] = w[2] = 0; return; }
    if (std::isinf(a)) { w[0] = sign | 0x7FFF; w[1] = w[2] = 0; return; }
    if (std::isnan(a)) { w[0] = sign | 0x7FFF; w[1] = 0xC0000000u; w[2] = 0; return; }
    int e;
    double m = std::frexp(a, &e);           // a = m * 2^e, m in [0.5, 1)
    u64 mant = (u64)std::ldexp(m, 64);      // top bit set
    w[0] = sign | (u32)((e - 1 + 16383) & 0x7FFF);
    w[1] = (u32)(mant >> 32);
    w[2] = (u32)mant;
}

static double from_ext(const u32 w[3]) {
    bool neg = w[0] & 0x80000000u;
    int e = (int)(w[0] & 0x7FFF);
    u64 mant = ((u64)w[1] << 32) | w[2];
    double r;
    if (e == 0x7FFF) r = (mant << 1) ? NAN : INFINITY;
    else if (mant == 0) r = 0.0;
    else r = std::ldexp((double)mant, e - 16383 - 63);
    return neg ? -r : r;
}

static double round_mode(double v, u32 mode) {
    switch (mode & 3) {
    case 0: return std::nearbyint(v);       // nearest (even)
    case 1: return std::ceil(v);            // +inf
    case 2: return std::floor(v);           // -inf
    default: return std::trunc(v);         // zero
    }
}

static double apply_precision(double v, u32 insn) {
    u32 prec = (bit(insn, 19) << 1) | bit(insn, 7);  // 0 single, 1 double, 2 extended
    return prec == 0 ? (double)(float)v : v;
}

bool fpa_exec(FPAState& s, ARM926& cpu, GuestMem& mem, u32 insn, u32 pc) {
    u32 cp = (insn >> 8) & 0xF;
    if (cp != 1 && cp != 2) return false;
    u32 type = (insn >> 25) & 7;

    // ---- data transfer: LDF/STF (cp 1), LFM/SFM (cp 2) ----
    if (type == 6) {
        u32 P = bit(insn, 24), U = bit(insn, 23), W = bit(insn, 21), L = bit(insn, 20);
        u32 rn = (insn >> 16) & 15, fd = (insn >> 12) & 7;
        u32 off = (insn & 0xFF) * 4;
        u32 base = rn == 15 ? pc + 8 : cpu.r[rn];
        u32 addr = P ? (U ? base + off : base - off) : base;
        if (cp == 1) {
            u32 prec = (bit(insn, 22) << 1) | bit(insn, 15);   // 0 single, 1 double, 2 extended, 3 packed
            if (L) {
                if (prec == 0) s.f[fd] = from_single(mem.r32(addr));
                else if (prec == 1) s.f[fd] = from_double(mem.r32(addr), mem.r32(addr + 4));
                else { u32 w[3] = {mem.r32(addr), mem.r32(addr + 4), mem.r32(addr + 8)}; s.f[fd] = from_ext(w); }
            } else {
                if (prec == 0) mem.w32(addr, to_single(s.f[fd]));
                else if (prec == 1) { u32 hi, lo; to_double(s.f[fd], &hi, &lo); mem.w32(addr, hi); mem.w32(addr + 4, lo); }
                else { u32 w[3]; to_ext(s.f[fd], w); mem.w32(addr, w[0]); mem.w32(addr + 4, w[1]); mem.w32(addr + 8, w[2]); }
            }
        } else {
            u32 n = (bit(insn, 22) << 1) | bit(insn, 15);
            if (n == 0) n = 4;
            for (u32 i = 0; i < n; i++) {
                u32 r = (fd + i) & 7, a = addr + i * 12;
                if (L) { u32 w[3] = {mem.r32(a), mem.r32(a + 4), mem.r32(a + 8)}; s.f[r] = from_ext(w); }
                else { u32 w[3]; to_ext(s.f[r], w); mem.w32(a, w[0]); mem.w32(a + 4, w[1]); mem.w32(a + 8, w[2]); }
            }
        }
        if ((W || !P) && rn != 15) cpu.r[rn] = U ? base + off : base - off;
        return true;
    }
    if (type != 7 || bit(insn, 24) || cp != 1) return false;

    u32 fm_i = insn & 7;
    double fm = bit(insn, 3) ? kConst[fm_i] : s.f[fm_i];
    u32 fn = (insn >> 16) & 7, fd = (insn >> 12) & 7;
    u32 op = (insn >> 20) & 15;

    // ---- register transfer ----
    if (bit(insn, 4)) {
        u32 rd = (insn >> 12) & 15;
        switch (op) {
        case 0: // FLT Fn, Rd
            s.f[fn] = apply_precision((double)(s32)(rd == 15 ? pc + 8 : cpu.r[rd]), insn);
            return true;
        case 1: { // FIX Rd, Fm
            double v = round_mode(fm, (insn >> 5) & 3);
            s32 r;
            if (std::isnan(v)) r = 0;
            else if (v >= 2147483647.0) r = INT_MAX;
            else if (v <= -2147483648.0) r = INT_MIN;
            else r = (s32)v;
            if (rd != 15) cpu.r[rd] = (u32)r;
            return true;
        }
        case 2: s.fpsr = cpu.r[rd]; return true;            // WFS
        case 3: if (rd != 15) cpu.r[rd] = s.fpsr; return true; // RFS
        case 4: return true;                                  // WFC
        case 5: if (rd != 15) cpu.r[rd] = 0; return true;     // RFC
        case 9: case 11: case 13: case 15: {                  // CMF, CNF, CMFE, CNFE
            double a = s.f[fn], b = (op == 11 || op == 15) ? -fm : fm;
            u32 nzcv;
            if (std::isnan(a) || std::isnan(b)) nzcv = 0x3;   // unordered: C V
            else if (a == b) nzcv = 0x6;                      // Z C
            else if (a < b) nzcv = 0x8;                       // N
            else nzcv = 0x2;                                  // C
            cpu.cpsr = (cpu.cpsr & 0x0FFFFFFF) | (nzcv << 28);
            return true;
        }
        }
        return false;
    }

    // ---- data operations ----
    double r;
    if (bit(insn, 15)) { // monadic
        switch (op) {
        case 0: case 14: case 15: r = fm; break;            // MVF, URD, NRM
        case 1: r = -fm; break;                             // MNF
        case 2: r = std::fabs(fm); break;                   // ABS
        case 3: r = round_mode(fm, (insn >> 5) & 3); break; // RND
        case 4: r = std::sqrt(fm); break;                   // SQT
        case 5: r = std::log10(fm); break;                  // LOG
        case 6: r = std::log(fm); break;                    // LGN
        case 7: r = std::exp(fm); break;                    // EXP
        case 8: r = std::sin(fm); break;
        case 9: r = std::cos(fm); break;
        case 10: r = std::tan(fm); break;
        case 11: r = std::asin(fm); break;
        case 12: r = std::acos(fm); break;
        default: r = std::atan(fm); break;                  // 13: ATN
        }
    } else {
        double a = s.f[fn];
        switch (op) {
        case 0: r = a + fm; break;                          // ADF
        case 1: case 9: r = a * fm; break;                  // MUF, FML
        case 2: r = a - fm; break;                          // SUF
        case 3: r = fm - a; break;                          // RSF
        case 4: case 10: r = a / fm; break;                 // DVF, FDV
        case 5: case 11: r = fm / a; break;                 // RDF, FRD
        case 6: r = std::pow(a, fm); break;                 // POW
        case 7: r = std::pow(fm, a); break;                 // RPW
        case 8: r = std::remainder(a, fm); break;           // RMF
        case 12: r = std::atan2(a, fm); break;              // POL
        default: return false;
        }
        if (op >= 9 && op <= 11) r = (double)(float)r;      // fast single-precision ops
    }
    s.f[fd] = apply_precision(r, insn);
    return true;
}

} // namespace gp2x
