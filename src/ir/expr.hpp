#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir/ops.hpp"

namespace sopt {

struct Node {
  Op op = Op::Const;
  Type type = Type::Float;
  uint8_t nargs = 0;                 // number of operands (Construct: 1..4)
  uint8_t swz[4] = {0, 0, 0, 0};     // Op::Swizzle: source component per result component
  uint32_t args[4] = {0, 0, 0, 0};
  float value[4] = {0, 0, 0, 0};     // Op::Const, width(type) components
  uint32_t input = 0;                // Op::Input
};

// Hash-consed DAG. Nodes are topologically ordered (args always precede users).
struct Expr {
  std::vector<Node> nodes;
  uint32_t root = 0;
};

struct InputDecl {
  std::string name;
  double lo = 0.0;
  double hi = 1.0;
  uint32_t grid = 0;  // 0 = continuous, N = values lo + k*(hi-lo)/N
  Type type = Type::Float;  // float1..4; every component has the same domain
  // A compile-time constant (a preprocessor definition the user can change): the
  // compiler folds expressions of these, so they cost nothing.
  bool compileTime = false;
  double value = 0.0;  // compileTime: the current value (a preprocessor definition's)
  // Where the value comes from (owner, 2026-10-08: group math by rate so the compiler can
  // precompute and schedule it): a uniform (a constant in ReShade's performance mode), a
  // per-pixel value (interpolant, position, earlier result) or a texture fetch, which arrives
  // late; fetchOrder: its position among the region's fetches in the source.
  enum class Rate : uint8_t { Pixel, Uniform, Fetch } rate = Rate::Pixel;
  uint32_t fetchOrder = 0;
  // A uniform folded like a compile-time constant (the performance mode cost, see perfInputs).
  bool perfFolded = false;
  bool folds() const { return compileTime || perfFolded; }
};

// The inputs with every uniform folded like a compile-time constant (performance mode).
std::vector<InputDecl> perfInputs(const std::vector<InputDecl>& inputs);

// Error budget per output value (design 4.2). Color8/Color10: max difference in
// 8/10-bit code values after quantization. Texcoord: max deviation in pixels at 4K
// width by default (eps = px / width). Exact also covers conditions, temporal feedback and depth.
struct Budget {
  enum class Kind { Exact, Color8, Color10, Abs, Rel, Texcoord } kind = Kind::Exact;
  double eps = 0.0;     // Abs, Rel, Texcoord
  int maxCodeDiff = 1;  // Color8, Color10
  double px = 0.0;      // Texcoord: pixels at `width` wide (eps = px / width)
  double width = 3840.0;
  // Owner's accuracy rule: a candidate may also differ from the float32 original where
  // it is at least as close to the exact (real-number) value, or within the budget of
  // it (see pointAccurate). Never for Exact budgets.
  bool vsExact = true;
  // Less accurate variants (owner: listed with their accuracy, the user decides): a
  // factor > 1 also accepts candidates within `loose` times the budget (color budgets:
  // one more code) and `loose` times the original's own error. 0 = off.
  double loose = 0.0;
  // Rel: relative to max(|t|, S), S = the original's own rounding-error scale (see
  // relBase). Off where the float32 original does not follow exact math (driver).
  bool errorScale = true;
  bool scaledRel() const { return kind == Kind::Rel && errorScale; }
  int codeBits() const { return kind == Kind::Color10 ? 10 : 8; }
};

// Vector inputs are sampled per component: slot k of the point set is one scalar
// component. slotDecls lists them (name.x, name.y, ...), inputSlots gives the first
// slot of each input.
std::vector<InputDecl> slotDecls(const std::vector<InputDecl>& inputs);
std::vector<uint32_t> inputSlots(const std::vector<InputDecl>& inputs);

struct Program {
  std::vector<InputDecl> inputs;
  std::string outputName = "r";
  Expr target;
  Budget budget;
};

// Result type of an op node with the given operand types, or nullopt if they don't fit
// (see Shape). swzCount is the number of swizzle components.
std::optional<Type> inferType(Op op, const Type* args, unsigned nargs, unsigned swzCount = 0);

class ExprBuilder {
 public:
  uint32_t input(uint32_t index, Type type = Type::Float);
  uint32_t constant(float v);
  uint32_t constant(Type type, const float* v);
  uint32_t constantU(uint32_t v);  // a Type::Uint constant
  // Throws std::invalid_argument if the operand types don't fit the op.
  uint32_t op(Op op, uint32_t a, uint32_t b = 0, uint32_t c = 0);
  uint32_t swizzle(uint32_t a, const uint8_t* comps, unsigned count);
  uint32_t construct(const uint32_t* args, unsigned nargs);
  Expr finish(uint32_t root);
  const std::vector<Node>& nodes() const { return nodes_; }

 private:
  uint32_t intern(const Node& n);
  std::vector<Node> nodes_;
  std::unordered_map<uint64_t, std::vector<uint32_t>> map_;
};

// Static cost with sharing: every distinct node counts once. With contraction, an
// add/sub over a single-use mul (or div) costs model.fusedAdd.
uint32_t dagCost(const Expr& e, const CostModel& model = defaultCostModel());
// The same with compile-time inputs (InputDecl::folds): nodes computed only from
// constants and compile-time inputs are folded by the compiler and cost nothing.
uint32_t dagCost(const Expr& e, const CostModel& model, const std::vector<InputDecl>& inputs);
// Divisions as the compilers lower them (RGA and ptxas, 2026-10-09): a / b = a * rcp(b) with one
// reciprocal per distinct divisor b, shared by every division by b and by rcp(b) itself, and none for a
// compile-time b (ct, from compileTimeNodes: x / 3.0 is x * 0.33333334). Per Div node its cost, 0 elsewhere.
std::vector<uint32_t> divCosts(const Expr& e, const CostModel& model, const std::vector<bool>& ct);
// That cost per node (dagCost(e, model, inputs) is the sum).
std::vector<uint32_t> nodeCosts(const Expr& e, const CostModel& model, const std::vector<InputDecl>& inputs);
// Per node: computed only from constants and compile-time inputs (InputDecl::folds).
std::vector<bool> compileTimeNodes(const Expr& e, const std::vector<InputDecl>& inputs);
// Secondary measures for scheduling (owner, 2026-10-08), like registers: they break ties and
// make "better scheduling" / "faster in performance mode" variants, never the main cost.
// perfCost: dagCost with uniforms folded (ReShade's performance mode). tail: the cost of the
// nodes that depend on the last texture fetch (the highest InputDecl::fetchOrder among the
// fetches e reads), i.e. the work left once the last sample arrives; 0 without fetches.
// critical: the costliest chain of dependent nodes. tail and critical use the costs of
// nodeCosts(e, model, inputs).
struct ScheduleMetrics {
  uint32_t perfCost = 0;
  uint32_t tail = 0;
  uint32_t critical = 0;
};
ScheduleMetrics scheduleMetrics(const Expr& e, const CostModel& model, const std::vector<InputDecl>& inputs);
// Whether a program has inputs the secondary measures can tell apart (uniforms or fetches).
bool hasScheduleInputs(const std::vector<InputDecl>& inputs);
std::vector<uint32_t> useCounts(const Expr& e);
// For an add/sub node: the operand index (0/1) that contracts into an fma, else -1.
int fusedArg(const Expr& e, uint32_t node, const std::vector<uint32_t>& uses, bool divIsMul);
// CostModel::amdFolds: per node, whether it folds into another instruction (cost 1 per component):
// a mul by a constant +-2 / +-4 / +-0.5 whose other operand is a single-use, same-width result of
// an instruction with output modifiers (and that is not itself contracted into an fma), or a
// min / max with a single-use min / max operand that does not fold itself (three operands).
std::vector<bool> amdFoldedNodes(const Expr& e, const std::vector<uint32_t>& uses, const CostModel& m);
// The ops whose instruction takes an output modifier (omod) in amdFoldedNodes.
bool takesOmod(Op op);
bool isOmodScale(float v);
bool containsInexact(const Expr& e);
bool containsOp(const Expr& e, Op op);
// Whether e relies on exact float rounding that fxc -O3 would reassociate away: (v + c) - c,
// (v + c) + -c or mad(a, b, c) - c with |c| >= 2^22 (the add-round trick). Such forms must be
// written as precise.
bool needsPrecise(const Expr& e);
Type nodeType(const Expr& e, uint32_t node);
unsigned operandCount(const Node& n);
std::string toString(const Expr& e, const std::vector<InputDecl>& inputs);
// The same in GLSL: fract, mix, inversesqrt, roundEven, clamp(x, 0.0, 1.0) for saturate, 1.0 / x
// for rcp, a * b + c for mad, vecN, floatBitsToUint / uintBitsToFloat, vecN(s) for scalar
// operands GLSL does not broadcast (pow(v, vec3(2.0))).
std::string toGlsl(const Expr& e, const std::vector<InputDecl>& inputs);
std::string formatFloat(float v);
std::string formatUint(uint32_t v);  // 13u, 0x3F800000u

// HLSL intrinsics that are not ops but written out the way DXC lowers them: radians(x) =
// x * (pi / 180), degrees(x) = x * (180 / pi), log10(x) = log2(x) * (ln 2 / ln 10), tan(x) =
// sin(x) / cos(x), cross(a, b) = a.yzx * b.zxy - a.zxy * b.yzx (float3). isSugarCall says
// whether buildSugarCall knows the name and arity; buildSugarCall throws
// std::invalid_argument when the operand types don't fit.
bool isSugarCall(std::string_view name, size_t arity);
uint32_t buildSugarCall(ExprBuilder& b, std::string_view name, const uint32_t* args, size_t arity);

}  // namespace sopt
