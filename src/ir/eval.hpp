#pragma once
#include <cstddef>

#include <vector>

#include "ir/expr.hpp"

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

// One node over n points, vector aware. arg[k][c] is component c of operand k (a
// float1 operand only has c = 0 and is broadcast); out[c] receives component c of the
// result. tmp is scratch. Not for Input/Const. dot is summed left to right, fused
// (fma chain) under madFused profiles; normalize is v * (1 / sqrt(dot(v, v))).
void evalNode(const Node& node, const Type* argTypes, const float* const (*arg)[4],
              float* const* out, size_t n, const Profile& profile, std::vector<float>& tmp);

// Copies node i of src into b (operands already mapped by `map`); a node whose operands
// are all constants is folded into a constant (reference profile).
uint32_t foldCopyNode(const Expr& src, uint32_t i, const std::vector<uint32_t>& map, ExprBuilder& b);

// e with the compile-time inputs replaced by their values (InputDecl::value) and
// constant subexpressions folded, as the shader compiler does. The other inputs are
// renumbered: `remaining` receives them, oldIndex (optional) their original indices.
Expr specializeCompileTime(const Expr& e, const std::vector<InputDecl>& inputs,
                           std::vector<InputDecl>& remaining, std::vector<uint32_t>* oldIndex = nullptr);

}  // namespace sopt
