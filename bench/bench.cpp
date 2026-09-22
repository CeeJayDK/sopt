// Benchmark of the optimizer itself (design section 6).
//  - Example suite: every *.sopt with a "# expect:" line must reach that cost.
//  - Planted problems: a random cheap program is obfuscated with known
//    cost-increasing rewrites; the search should recover the planted cost.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "ir/parser.hpp"
#include "search/driver.hpp"

using namespace sopt;

namespace {

struct Row {
  std::string name;
  uint32_t targetCost = 0;
  uint32_t goalCost = 0;
  uint32_t bestCost = 0;
  bool found = false;
  std::string note;
  RunResult r;
};

void printHeader() {
  std::printf("%-18s %6s %5s %5s %3s %4s %9s %8s %8s %8s %8s  %s\n", "problem", "target", "goal",
              "best", "ok", "iter", "bank", "gen M/s", "first s", "search s", "verify s", "note");
}

void printRow(const Row& row) {
  const auto& s = row.r.search;
  const double rate = s.seconds > 0 ? s.generated / s.seconds / 1e6 : 0.0;
  std::printf("%-18s %6u %5u ", row.name.c_str(), row.targetCost, row.goalCost);
  if (row.found) std::printf("%5u ", row.bestCost);
  else std::printf("%5s ", "-");
  std::printf("%3s %4u %9llu %8.2f %8.3f %8.3f %8.3f  %s%s\n",
              row.found && row.bestCost <= row.goalCost ? "yes" : "NO", row.r.iterations,
              (unsigned long long)s.bankSize, rate, s.firstHitSec, row.r.searchSec,
              row.r.verifySec, s.limitHit ? "limit " : "", row.note.c_str());
}

Row runOne(const std::string& name, const Program& prog, uint32_t goalCost, const Options& opt) {
  Row row;
  row.name = name;
  row.goalCost = goalCost;
  row.r = optimize(prog, opt);
  row.targetCost = row.r.targetCost;
  row.found = !row.r.accepted.empty();
  if (row.found) row.bestCost = row.r.accepted[0].cost;
  return row;
}

// ---- planted problems ------------------------------------------------------

uint32_t randomProgram(ExprBuilder& b, Rng& rng, uint32_t numInputs, uint32_t steps) {
  std::vector<uint32_t> pool;
  for (uint32_t i = 0; i < numInputs; ++i) pool.push_back(b.input(i));
  pool.push_back(b.constant(0.5f));
  pool.push_back(b.constant(2.0f));
  const Op ops[] = {Op::Add, Op::Sub, Op::Mul, Op::Min, Op::Max, Op::Lerp, Op::Mad, Op::Abs};
  uint32_t last = pool[0];
  for (uint32_t s = 0; s < steps; ++s) {
    const Op op = ops[rng.next() % (sizeof(ops) / sizeof(ops[0]))];
    auto pick = [&]() {
      // Prefer recent values to get depth rather than a flat sum.
      const size_t n = pool.size();
      return (rng.next() & 1) ? pool[n - 1 - rng.next() % std::min<size_t>(n, 2)]
                              : pool[rng.next() % n];
    };
    const uint32_t a = pick(), bb = pick(), c = pick();
    const uint8_t arity = info(op).arity;
    bool allConst = true;
    const uint32_t args[3] = {a, bb, c};
    for (uint8_t k = 0; k < arity; ++k) allConst = allConst && b.nodes()[args[k]].op == Op::Const;
    const bool degenerate = arity >= 2 && a == bb;  // min(x, x), x - x, lerp(x, x, t), ...
    if (allConst || degenerate) {
      --s;  // draw again (constant expressions would be folded by any compiler)
      continue;
    }
    last = b.op(op, a, bb, c);
    pool.push_back(last);
  }
  return last;
}

// Cost if shared subexpressions were duplicated (what v1's tree-cost bank can represent).
// In a tree every node has one use, so any add/sub over a mul contracts.
uint32_t treeCost(const Expr& e, uint32_t idx, const CostModel& m) {
  const Node& n = e.nodes[idx];
  uint32_t c = m[n.op];
  if (n.op == Op::Add || n.op == Op::Sub)
    for (int k = 0; k < 2; ++k)
      if (m.fusesIntoAdd(e.nodes[n.args[k]].op)) c = m.fusedAdd;
  for (uint8_t k = 0; k < info(n.op).arity; ++k) c += treeCost(e, n.args[k], m);
  return c;
}

// Rebuilds e with cost-increasing but semantically equivalent (up to rounding) rewrites.
Expr obfuscate(const Expr& e, Rng& rng) {
  ExprBuilder b;
  std::vector<uint32_t> map(e.nodes.size());
  for (size_t i = 0; i < e.nodes.size(); ++i) {
    const Node& n = e.nodes[i];
    const uint32_t a = map[n.args[0]], c1 = map[n.args[1]], c2 = map[n.args[2]];
    const bool doit = (rng.next() % 100) < 70;
    uint32_t out;
    switch (n.op) {
      case Op::Input: out = b.input(n.input); break;
      case Op::Const: out = b.constant(n.value); break;
      case Op::Lerp:  // lerp(a, b, t) -> a + t * (b - a)
        out = doit ? b.op(Op::Add, a, b.op(Op::Mul, c2, b.op(Op::Sub, c1, a))) : b.op(n.op, a, c1, c2);
        break;
      case Op::Mad:  // mad(a, b, c) -> a * b + c
        out = doit ? b.op(Op::Add, b.op(Op::Mul, a, c1), c2) : b.op(n.op, a, c1, c2);
        break;
      case Op::Mul:
        if (doit && a == c1) {  // x * x -> pow(x, 2)
          out = b.op(Op::Pow, a, b.constant(2.0f));
        } else if (doit && b.nodes()[c1].op == Op::Add) {  // x * (y + z) -> x*y + x*z
          const Node add = b.nodes()[c1];
          out = b.op(Op::Add, b.op(Op::Mul, a, add.args[0]), b.op(Op::Mul, a, add.args[1]));
        } else {
          out = b.op(n.op, a, c1);
        }
        break;
      case Op::Add:  // x + y -> x - (-y)
        out = doit ? b.op(Op::Sub, a, b.op(Op::Neg, c1)) : b.op(n.op, a, c1);
        break;
      default: out = b.op(n.op, a, c1, c2); break;
    }
    map[i] = out;
  }
  return b.finish(map[e.root]);
}

}  // namespace

int main(int argc, char** argv) {
  std::string examples;
  uint32_t planted = 0, size = 3, inputs = 3;
  uint64_t seed = 1;
  Options opt;
  opt.v1Points = 1u << 18;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : (std::exit(2), argv[0]); };
    if (a == "--examples") examples = next();
    else if (a == "--planted") planted = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--size") size = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--inputs") inputs = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
    else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--v1") opt.v1Points = std::strtoull(next(), nullptr, 10);
    else if (a == "--time") opt.search.timeLimitSec = std::strtod(next(), nullptr);
    else if (a == "--max-bank") opt.search.maxBank = std::strtoull(next(), nullptr, 10);
    else if (a == "--no-affine") opt.search.affine = false;
    else if (a == "--no-inner") opt.search.inner = false;
    else if (a == "--helpers") opt.search.helpers = true;
    else if (a == "--order-model") {
      opt.search.order = costModelByName(next());
      if (!opt.search.order) {
        std::puts("unknown cost model (generic, rdna3)");
        return 2;
      }
    }
    else if (a == "--cost-model") {
      opt.search.model = costModelByName(next());
      if (!opt.search.model) {
        std::puts("unknown cost model (generic, rdna3)");
        return 2;
      }
    } else {
      std::puts("usage: sopt-bench [--examples DIR] [--planted N --size K --inputs I] [--seed S]\n"
                "                  [--v1 N] [--time S] [--max-bank N] [--cost-model M] [--order-model M] [--no-affine] [--no-inner] [--helpers]");
      return 2;
    }
  }
  if (examples.empty() && planted == 0) examples = "examples";
  const CostModel& model = *opt.search.model;
  std::printf("cost model: %s%s%s\n", std::string(model.name).c_str(),
              opt.search.order ? (", order " + std::string(opt.search.order->name)).c_str() : "",
              opt.search.affine ? (opt.search.inner ? ", affine, inner" : ", affine") : "");
  if (opt.search.helpers) std::printf("helpers: lerp/step enumerated\n");

  int failures = 0, knownLimit = 0;
  printHeader();
  if (!examples.empty()) {
    std::vector<std::string> files;
    for (const auto& ent : std::filesystem::directory_iterator(examples))
      if (ent.path().extension() == ".sopt") files.push_back(ent.path().string());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      const Program prog = loadProgram(f);
      const std::string expect = readExpect(f);
      const uint32_t goal = expect.empty() ? dagCost(prog.target, model) - 1
                                           : dagCost(parseExpr(expect, prog.inputs), model);
      const Row row = runOne(std::filesystem::path(f).stem().string(), prog, goal, opt);
      printRow(row);
      failures += !(row.found && row.bestCost <= row.goalCost);
    }
  }

  Rng rng(seed);
  uint32_t attempts = 0;
  for (uint32_t p = 0; p < planted && attempts < planted * 100; ++attempts) {
    Program prog;
    const char* names[] = {"a", "b", "c", "d", "e", "f"};
    for (uint32_t i = 0; i < inputs && i < 6; ++i) prog.inputs.push_back({names[i], 0.0, 1.0, 255});
    prog.budget.kind = Budget::Kind::Rel;
    prog.budget.eps = 1e-5;
    ExprBuilder b;
    const Expr plantedExpr = b.finish(randomProgram(b, rng, inputs, size));
    prog.target = obfuscate(plantedExpr, rng);
    const uint32_t goal = dagCost(plantedExpr, model);
    if (dagCost(prog.target, model) <= goal) continue;  // not more expensive; draw again
    Row row = runOne("planted_" + std::to_string(p++), prog, goal, opt);
    const bool needsSharing = treeCost(plantedExpr, plantedExpr.root, model) > goal;
    if (needsSharing) row.note = "needs-sharing";
    printRow(row);
    if (!(row.found && row.bestCost <= row.goalCost)) {
      if (needsSharing) {
        ++knownLimit;  // DAG sharing is a documented v1 limitation (M7)
        continue;
      }
      ++failures;
      std::printf("    planted: %s\n    target:  %s\n", toString(plantedExpr, prog.inputs).c_str(),
                  row.r.targetText.c_str());
    }
  }
  std::printf("\n%d problem(s) not recovered, %d not recovered due to DAG sharing (known v1 limit)\n",
              failures, knownLimit);
  return failures ? 1 : 0;
}
