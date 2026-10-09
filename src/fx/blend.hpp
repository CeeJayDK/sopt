#pragma once
#include <vector>

#include "fx/classic.hpp"
#include "fx/frontend.hpp"

namespace sopt::fx {

// The final blend with the back buffer moved to the blend stage (owner, 2026-10-08, classical step
// 3). A pixel shader whose passes all draw to the back buffer with PostProcessVS and no blend state,
// and whose return value is, per color channel, A + B * d (d = the back buffer read at the pixel's
// own texcoord, A and B not depending on it) or min / max(d, X), returns the source of a blend
// instead:
//   B scalar        float4(A, B)  SrcBlend ONE, DestBlend SRCALPHA   (lerp(d, X, t): A = X * t, B = 1 - t)
//   A = 0           B             DESTCOLOR, ZERO                    (multiply)
//   B = 1           A             ONE, ONE                           (add)
//   B = 1 - A       A             ONE, INVSRCCOLOR                   (screen)
//   min / max       X             BlendOp MIN / MAX
// The destination alpha is kept (SrcBlendAlpha ZERO, DestBlendAlpha ONE), so the original must
// return float3 or float4(..., d.a). If d is read nowhere else the compiler drops the texture fetch.
// Verified like a variant (the source clamped to [0, 1] on 8 / 10-bit back buffers, not on scRGB);
// a format it fails on gets the original. Switch SOPT_<file>_B<line>; sopt-fx keeps it only where
// --isa measures fewer pixel shader instructions (the blend unit's own cost is not measured).
// Regions are extracted again with opt (down to one operation) for the candidate pixel shaders.
std::vector<SourceRewrite> blendRewrites(const Effect& fx, const RegionOptions& opt, uint64_t seed);

}  // namespace sopt::fx
