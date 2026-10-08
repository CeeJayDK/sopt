#pragma once
#include <vector>

#include "fx/classic.hpp"
#include "fx/frontend.hpp"
#include "ir/ops.hpp"

namespace sopt::fx {

// Lower frequency (owner, 2026-10-08, classical step 2): work moved from every pixel to the three
// vertices of a full-screen pass.
//   (a) math of uniforms only (sin / cos of an angle, products of sliders) becomes a flat
//       (nointerpolation) vertex shader output; in performance mode the compiler folds it anyway,
//       so those statements switch only with !__RESHADE_PERFORMANCE_MODE__ (and D3D10+: SM3 has no
//       flat interpolation);
//   (b) math affine in the interpolated inputs (rotated / scaled texture coordinates) becomes an
//       interpolated vertex shader output: interpolation of an affine function is exact.
// Per pixel shader whose passes all use one vertex shader (void, plain parameters): its regions'
// largest such subexpressions worth more than an interpolation (a few instructions per component)
// go into float4 outputs of a wrapper vertex shader (sopt_VS_<PS>: the original one, then the
// values), the pixel shader gets them as extra parameters, the passes use the wrapper. One rewrite
// per pixel shader (switch SOPT_<file>_V<line>); sopt-fx keeps it only where --isa measures a gain.
std::vector<SourceRewrite> hoistRewrites(const Effect& fx, const std::vector<Region>& regions, const CostModel& m);

}  // namespace sopt::fx
