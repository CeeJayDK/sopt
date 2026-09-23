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
};

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
// The same with compile-time inputs (InputDecl::compileTime): nodes computed only from
// constants and compile-time inputs are folded by the compiler and cost nothing.
uint32_t dagCost(const Expr& e, const CostModel& model, const std::vector<InputDecl>& inputs);
// Per node: computed only from constants and compile-time inputs.
std::vector<bool> compileTimeNodes(const Expr& e, const std::vector<InputDecl>& inputs);
std::vector<uint32_t> useCounts(const Expr& e);
// For an add/sub node: the operand index (0/1) that contracts into an fma, else -1.
int fusedArg(const Expr& e, uint32_t node, const std::vector<uint32_t>& uses, bool divIsMul);
bool containsInexact(const Expr& e);
bool containsOp(const Expr& e, Op op);
Type nodeType(const Expr& e, uint32_t node);
unsigned operandCount(const Node& n);
std::string toString(const Expr& e, const std::vector<InputDecl>& inputs);
std::string formatFloat(float v);

}  // namespace sopt
