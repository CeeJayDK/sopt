#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir/ops.hpp"

namespace sopt {

struct Node {
  Op op = Op::Const;
  uint32_t args[3] = {0, 0, 0};
  float value = 0.0f;  // Op::Const
  uint32_t input = 0;  // Op::Input
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
};

struct Budget {
  enum class Kind { Exact, Color8, Abs, Rel } kind = Kind::Exact;
  double eps = 0.0;
  int maxCodeDiff = 1;  // Color8 only
};

struct Program {
  std::vector<InputDecl> inputs;
  std::string outputName = "r";
  Expr target;
  Budget budget;
};

class ExprBuilder {
 public:
  uint32_t input(uint32_t index);
  uint32_t constant(float v);
  uint32_t op(Op op, uint32_t a, uint32_t b = 0, uint32_t c = 0);
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
std::vector<uint32_t> useCounts(const Expr& e);
// For an add/sub node: the operand index (0/1) that contracts into an fma, else -1.
int fusedArg(const Expr& e, uint32_t node, const std::vector<uint32_t>& uses, bool divIsMul);
bool containsInexact(const Expr& e);
bool containsOp(const Expr& e, Op op);
Type nodeType(const Expr& e, uint32_t node);
std::string toString(const Expr& e, const std::vector<InputDecl>& inputs);
std::string formatFloat(float v);

}  // namespace sopt
