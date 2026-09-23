#include <bit>
#include <cinttypes>
#include <cmath>
#include <vector>

#include "ir/eval.hpp"
#include "test.hpp"
#include "ir/parser.hpp"
#include "verify/points.hpp"
#include "verify/verify.hpp"

using namespace sopt;

namespace {

float ev(Op op, float a, float b = 0.0f, float c = 0.0f, const Profile& p = kProfileRef) {
  return evalScalar(op, a, b, c, p);
}

bool same(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

std::vector<float> hashInputs() {
  std::vector<float> v = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 3.0f, 0.1f, 1e-7f, 1e-30f,
                          1.17549435e-38f, 1.4e-45f, 16777216.0f, 3.4e38f, -3.4e38f, 0.99999994f,
                          1.00000012f, 255.0f, 1.0f / 255.0f, 0.2126f, 0.7152f, 0.0722f};
  Rng rng(12345);
  for (int i = 0; i < 40; ++i) v.push_back(static_cast<float>(rng.uniform() * 4.0 - 2.0));
  for (int i = 0; i < 20; ++i) v.push_back(static_cast<float>(std::ldexp(rng.uniform(), -20 + i)));
  return v;
}

uint64_t hashOps(const std::vector<Op>& ops, const Profile& prof) {
  const auto in = hashInputs();
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](float f) {
    uint32_t bits = std::isnan(f) ? 0x7fc00000u : std::bit_cast<uint32_t>(f);
    h ^= bits;
    h *= 1099511628211ull;
  };
  for (Op op : ops) {
    const auto& oi = info(op);
    if (oi.arity == 1) {
      for (float a : in) mix(ev(op, a, 0, 0, prof));
    } else if (oi.arity == 2) {
      for (float a : in)
        for (float b : in) mix(ev(op, a, b, 0, prof));
    } else {
      for (size_t i = 0; i < in.size(); ++i)
        for (size_t j = 0; j < in.size(); j += 3)
          for (size_t k = 0; k < in.size(); k += 5) {
            float c = in[k];
            if (op == Op::Select) c = (k % 2) ? 1.0f : 0.0f;
            mix(op == Op::Select ? ev(op, c, in[i], in[j], prof) : ev(op, in[i], in[j], c, prof));
          }
    }
  }
  return h;
}

}  // namespace

TEST(eval_semantics) {
  CHECK(ev(Op::Step, 0.5f, 0.5f) == 1.0f);   // step(edge, x) = x >= edge
  CHECK(ev(Op::Step, 0.5f, 0.49f) == 0.0f);
  CHECK(ev(Op::Saturate, -2.0f) == 0.0f);
  CHECK(ev(Op::Saturate, 2.0f) == 1.0f);
  CHECK(ev(Op::Frac, -0.25f) == 0.75f);
  CHECK(ev(Op::Sign, -3.0f) == -1.0f);
  CHECK(ev(Op::Sign, 0.0f) == 0.0f);
  CHECK(ev(Op::Select, 1.0f, 2.0f, 3.0f) == 2.0f);
  CHECK(ev(Op::Select, 0.0f, 2.0f, 3.0f) == 3.0f);
  CHECK(ev(Op::Clamp, 5.0f, 0.0f, 1.0f) == 1.0f);
  CHECK(ev(Op::Mad, 2.0f, 3.0f, 4.0f) == 10.0f);
  CHECK(ev(Op::Lerp, 1.0f, 3.0f, 0.5f) == 2.0f);
}

TEST(eval_lerp_profiles_differ) {
  // HLSL lerp (a + t*(b-a)) and GLSL mix (a*(1-t) + b*t) round differently.
  int diffs = 0;
  Rng rng(7);
  for (int i = 0; i < 10000; ++i) {
    const float a = static_cast<float>(rng.uniform()), b = static_cast<float>(rng.uniform()),
                t = static_cast<float>(rng.uniform());
    if (!same(ev(Op::Lerp, a, b, t, kProfileRef), ev(Op::Lerp, a, b, t, kProfileMix))) ++diffs;
  }
  CHECK(diffs > 0);
  // mix hits b exactly at t = 1, lerp does not always.
  CHECK(ev(Op::Lerp, 0.1f, 0.7f, 1.0f, kProfileMix) == 0.7f);
}

TEST(eval_fma_differs) {
  const float a = 0.1f, b = 0.3f, c = -0.03f;
  CHECK(!same(ev(Op::Mad, a, b, c, kProfileRef), ev(Op::Mad, a, b, c, kProfileFma)));
}

// Cross-compiler check (M0 done criterion): these hashes must be identical on
// MSVC, GCC and Clang. If an exact op changes, regenerate the values.
TEST(eval_golden_exact_ops) {
  const std::vector<Op> exactOps = {Op::Neg, Op::Abs, Op::Saturate, Op::Floor, Op::Frac, Op::Sign,
                                    Op::Sqrt, Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Min, Op::Max,
                                    Op::Step, Op::Lt, Op::Le, Op::Gt, Op::Ge, Op::Eq, Op::Ne,
                                    Op::Mad, Op::Lerp, Op::Clamp, Op::Select};
  const uint64_t ref = hashOps(exactOps, kProfileRef);
  const uint64_t mix = hashOps({Op::Lerp}, kProfileMix);
  const uint64_t fma = hashOps({Op::Mad}, kProfileFma);
  std::printf("  hashes: ref=0x%016" PRIx64 " mix=0x%016" PRIx64 " fma=0x%016" PRIx64 "\n", ref,
              mix, fma);
  CHECK(ref == 0x7c1269b1ff7b7dd0ull);
  CHECK(mix == 0xb2700fe4fe4159deull);
  CHECK(fma == 0x674ea0f74ef564ebull);
}

TEST(eval_gpu_profile) {
  // a / b is a * rcp(b) on the GPU: differs from IEEE division for some inputs.
  int diffs = 0;
  Rng rng(3);
  for (int i = 0; i < 10000; ++i) {
    const float a = static_cast<float>(rng.uniform()), b = static_cast<float>(rng.uniform() + 0.1);
    diffs += !same(ev(Op::Div, a, b, 0, kProfileRef), ev(Op::Div, a, b, 0, kProfileGpu));
    CHECK(same(ev(Op::Div, a, b, 0, kProfileGpu), a * (1.0f / b)));
  }
  CHECK(diffs > 0);
  CHECK(ev(Op::Rcp, 4.0f) == 0.25f);
  // lerp contracted: fma(t, b - a, a).
  CHECK(same(ev(Op::Lerp, 0.1f, 0.7f, 0.3f, kProfileGpu), std::fma(0.3f, 0.7f - 0.1f, 0.1f)));
}

TEST(eval_gpu_contraction) {
  // The verifier contracts a single-use mul (or div) under add/sub into one fma.
  std::vector<InputDecl> in = {{"a", 0, 1, 0}, {"b", 0, 1, 0}, {"c", 0, 1, 0}};
  PointSet ps;
  ps.cols.resize(3);
  Rng rng(5);
  for (int i = 0; i < 1000; ++i)
    ps.add({static_cast<float>(rng.uniform()), static_cast<float>(rng.uniform()),
            static_cast<float>(rng.uniform() + 0.5)});
  struct Case {
    const char* text;
    float (*gpu)(float, float, float);
  };
  const Case cases[] = {
      {"a * b + c", [](float a, float b, float c) { return std::fma(a, b, c); }},
      {"a * b - c", [](float a, float b, float c) { return std::fma(a, b, -c); }},
      {"c - a * b", [](float a, float b, float c) { return std::fma(-a, b, c); }},
      {"a / c - b", [](float a, float b, float c) { return std::fma(a, 1.0f / c, -b); }},
      // shared product: not contracted
      {"a * b + a * b", [](float a, float b, float) { return a * b + a * b; }},
  };
  int contractedDiffs = 0;
  for (const auto& cs : cases) {
    const auto ref = evalAll(parseExpr(cs.text, in), ps, kProfileRef);
    const auto gpu = evalAll(parseExpr(cs.text, in), ps, kProfileGpu);
    for (size_t i = 0; i < ps.size(); ++i) {
      CHECK(same(gpu[i], cs.gpu(ps.cols[0][i], ps.cols[1][i], ps.cols[2][i])));
      contractedDiffs += !same(gpu[i], ref[i]);
    }
  }
  CHECK(contractedDiffs > 0);
}
