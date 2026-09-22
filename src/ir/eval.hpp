#pragma once
#include <cstddef>

#include "ir/ops.hpp"

namespace sopt {

// float32 reference semantics. Bool values are stored as 0.0f / 1.0f.
// Undefined inputs (NaN, pow of negative base, 0/0, ...) are don't-care and
// simply produce whatever the C++ float operation produces.
// Contraction across ops (profile.contract) needs the DAG and is done by the
// verifier's BlockEvaluator; here it only affects lerp.
float evalScalar(Op op, float a, float b, float c, const Profile& profile);

// Elementwise evaluation over n points. Unused operand pointers may be null.
void evalArray(Op op, const float* a, const float* b, const float* c, float* out, size_t n,
               const Profile& profile);

}  // namespace sopt
