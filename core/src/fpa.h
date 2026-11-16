// PocketPenguin - FPA floating-point emulation (what the Linux kernel's NWFPE did on the GP2X/Wiz).
// Old ARM Linux toolchains (OABI, glibc 2.3 era) used FPA instructions on coprocessors 1 and 2;
// the GP2X and Wiz have no FPA hardware, so the kernel emulated them on the undefined-instruction trap.
#pragma once
#include "arm926.h"

namespace gp2x {

class GuestMem;

struct FPAState {
    double f[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    u32 fpsr = 0x01000000; // system id: software emulator (NWFPE reports 0x01)
};

// Execute one FPA instruction. Returns true if 'insn' was an FPA instruction (and was executed).
bool fpa_exec(FPAState& s, ARM926& cpu, GuestMem& mem, u32 insn, u32 pc);

} // namespace gp2x
