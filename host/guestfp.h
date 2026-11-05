// Floating-point state for threads that run recompiled code.
#pragma once
#include "ppc_context.h"
#include <cstdlib>

// The recompiled code keeps a cached copy of MXCSR in ctx.fpscr.csr and only writes the register when it thinks the flush
// mode changes. It has to start as a copy of the real register: from zero, the first change also clears the exception masks.
// BCR2_NOFPINIT keeps the old zeroed cache, and BCR2_FORCEFTZ keeps denormals flushed for all floating-point code, both for comparison.
inline void guest_fp_init(PPCContext& ctx) {
    static const bool kNoInit = getenv("BCR2_NOFPINIT") != nullptr;
    static const bool kForce = getenv("BCR2_FORCEFTZ") != nullptr;
    if (kNoInit) return;
    ctx.fpscr.loadFromHost();
    if (kForce) ctx.fpscr.setcsr(ctx.fpscr.csr | PPCFPSCRRegister::FlushMask);   // the cache still says "off", so disableFlushMode leaves the register alone
}
