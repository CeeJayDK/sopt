#pragma once
#include <vector>

#include "fx/classic.hpp"
#include "fx/frontend.hpp"

namespace sopt::fx {

// Uniform zero guard (owner's open item, 2026-10-28): a pixel shader whose output equals an earlier value A whenever a
// uniform S is 0 (lerp(color, effect, Strength), color + effect * Amount, ...; within 4 float steps of the largest input,
// at sample points, the
// later values random) gets "if (S == 0.0) return A;" (out parameter: "{ out = A; return; }") right after A's last
// store, so the work after it is skipped. Only without performance mode: with it S is a constant and the compiler drops
// the work itself. A plain variable (or a swizzle of one) in the shader's top block, nothing but locals stored between.
// Switch SOPT_<file>_Z<line>; sopt-fx keeps it where --isa measures at most a few more instructions.
std::vector<SourceRewrite> zeroGuardRewrites(const Effect& fx, const RegionOptions& opt, uint64_t seed);

}  // namespace sopt::fx
