#include "measure/sass.hpp"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "measure/tools.hpp"

namespace sopt {
namespace {

namespace fs = std::filesystem;

std::string imm(float v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0f%08X", std::bit_cast<uint32_t>(v));
  return buf;
}

}  // namespace

int mufuWeight(int sm) { return sm == 75 || sm == 80 ? 4 : 8; }

// Mirrors what graphics compilers emit: approximate transcendentals, a / b as
// a * rcp(b), and add/sub/mul without a rounding modifier so ptxas may contract
// them into FFMA (as drivers do).
std::string emitPtx(const Expr& e, const std::vector<InputDecl>& inputs, int sm) {
  std::ostringstream body;
  const size_t nn = e.nodes.size();
  std::vector<std::string> reg(nn);
  int tmp = 0;
  auto t = [&]() { return "%t" + std::to_string(tmp++); };
  for (size_t i = 0; i < nn; ++i) {
    const Node& n = e.nodes[i];
    if (n.op == Op::Const) {
      reg[i] = imm(n.value);
      continue;
    }
    if (n.op == Op::Input) {
      reg[i] = "%f" + std::to_string(i);
      // Each input is loaded once and also stored back: its value must live in a
      // register, like a texture sample in a shader. Otherwise ptxas turns a select of
      // two inputs into a conditional (re)load and the FSEL disappears.
      body << "  ld.volatile.global.f32 " << reg[i] << ", [%a1+" << 4 * n.input << "];\n";
      body << "  st.global.f32 [%a2+" << 4 * (n.input + 1) << "], " << reg[i] << ";\n";
      continue;
    }
    const bool isBool = info(n.op).result == Type::Bool;
    const std::string d = (isBool ? "%p" : "%f") + std::to_string(i);
    reg[i] = d;
    const std::string a = reg[n.args[0]], b = reg[n.args[1]], c = reg[n.args[2]];
    std::string x, y;
    switch (n.op) {
      case Op::Neg: body << "  neg.f32 " << d << ", " << a << ";\n"; break;
      case Op::Abs: body << "  abs.f32 " << d << ", " << a << ";\n"; break;
      case Op::Saturate: body << "  cvt.sat.f32.f32 " << d << ", " << a << ";\n"; break;
      case Op::Floor: body << "  cvt.rmi.f32.f32 " << d << ", " << a << ";\n"; break;
      case Op::Frac:
        x = t();
        body << "  cvt.rmi.f32.f32 " << x << ", " << a << ";\n  sub.f32 " << d << ", " << a
             << ", " << x << ";\n";
        break;
      case Op::Sign:
        x = t(), y = t();
        body << "  set.gt.f32.f32 " << x << ", " << a << ", 0f00000000;\n  set.lt.f32.f32 " << y
             << ", " << a << ", 0f00000000;\n  sub.f32 " << d << ", " << x << ", " << y << ";\n";
        break;
      case Op::Sqrt: body << "  sqrt.approx.ftz.f32 " << d << ", " << a << ";\n"; break;
      case Op::Rsqrt: body << "  rsqrt.approx.ftz.f32 " << d << ", " << a << ";\n"; break;
      case Op::Rcp: body << "  rcp.approx.ftz.f32 " << d << ", " << a << ";\n"; break;
      case Op::Exp:  // exp(x) = exp2(x * log2(e))
        x = t();
        body << "  mul.f32 " << x << ", " << a << ", 0f3FB8AA3B;\n  ex2.approx.ftz.f32 " << d
             << ", " << x << ";\n";
        break;
      case Op::Log:  // log(x) = log2(x) * ln(2)
        x = t();
        body << "  lg2.approx.ftz.f32 " << x << ", " << a << ";\n  mul.f32 " << d << ", " << x
             << ", 0f3F317218;\n";
        break;
      case Op::Sin: body << "  sin.approx.ftz.f32 " << d << ", " << a << ";\n"; break;
      case Op::Cos: body << "  cos.approx.ftz.f32 " << d << ", " << a << ";\n"; break;
      case Op::Add: body << "  add.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Sub: body << "  sub.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Mul: body << "  mul.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Div:
        x = t();
        body << "  rcp.approx.ftz.f32 " << x << ", " << b << ";\n  mul.f32 " << d << ", " << a
             << ", " << x << ";\n";
        break;
      case Op::Min: body << "  min.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Max: body << "  max.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Step:  // step(edge, x) = x >= edge
        body << "  set.ge.f32.f32 " << d << ", " << b << ", " << a << ";\n";
        break;
      case Op::Pow:
        x = t(), y = t();
        body << "  lg2.approx.ftz.f32 " << x << ", " << a << ";\n  mul.f32 " << y << ", " << x
             << ", " << b << ";\n  ex2.approx.ftz.f32 " << d << ", " << y << ";\n";
        break;
      case Op::Lt: body << "  setp.lt.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Le: body << "  setp.le.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Gt: body << "  setp.gt.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Ge: body << "  setp.ge.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Eq: body << "  setp.eq.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Ne: body << "  setp.neu.f32 " << d << ", " << a << ", " << b << ";\n"; break;
      case Op::Mad:
        body << "  fma.rn.f32 " << d << ", " << a << ", " << b << ", " << c << ";\n";
        break;
      case Op::Lerp:  // a + t * (b - a)
        x = t();
        body << "  sub.f32 " << x << ", " << b << ", " << a << ";\n  fma.rn.f32 " << d << ", "
             << c << ", " << x << ", " << a << ";\n";
        break;
      case Op::Clamp:
        x = t();
        body << "  max.f32 " << x << ", " << a << ", " << b << ";\n  min.f32 " << d << ", " << x
             << ", " << c << ";\n";
        break;
      case Op::Select:
        body << "  selp.f32 " << d << ", " << b << ", " << c << ", " << a << ";\n";
        break;
      case Op::Input:
      case Op::Const:
      case Op::Count: break;
    }
  }
  std::string root = reg[e.root];
  if (e.nodes[e.root].op == Op::Const) {  // store needs a register
    body << "  mov.f32 %r, " << root << ";\n";
    root = "%r";
  }
  std::ostringstream s;
  s << "// Generated by sopt: SASS measurement of one candidate.\n"
    << ".version 8.7\n.target sm_" << sm << "\n.address_size 64\n\n"
    << ".visible .entry sopt(.param .u64 pin, .param .u64 pout)\n{\n"
    << "  .reg .u64 %a<3>;\n  .reg .f32 %f<" << nn << ">;\n  .reg .f32 %t<" << tmp + 1
    << ">;\n  .reg .f32 %r;\n  .reg .pred %p<" << nn << ">;\n"
    << "  ld.param.u64 %a1, [pin];\n  ld.param.u64 %a2, [pout];\n"
    << "  cvta.to.global.u64 %a1, %a1;\n  cvta.to.global.u64 %a2, %a2;\n"
    << body.str() << "  st.global.f32 [%a2], " << root << ";\n  ret;\n}\n";
  (void)inputs;
  return s.str();
}

SassCost parseSass(const std::string& disasm, int sm) {
  SassCost c;
  std::istringstream in(disasm);
  std::string line;
  bool any = false;
  while (std::getline(in, line)) {
    // Instruction lines look like "  /*0080*/  FADD R0, R2, 0.5 ;"
    const size_t addr = line.find("/*");
    const size_t close = addr == std::string::npos ? addr : line.find("*/", addr);
    if (close == std::string::npos || line.find_first_not_of(" \t") != addr) continue;
    std::string ins = line.substr(close + 2);
    const size_t semi = ins.find(';');
    if (semi == std::string::npos) continue;
    ins = ins.substr(0, semi);
    const size_t b = ins.find_first_not_of(" \t");
    if (b == std::string::npos) continue;
    ins = ins.substr(b);
    while (!ins.empty() && ins.back() == ' ') ins.pop_back();
    any = true;
    std::string op = ins.substr(0, ins.find(' '));
    if (op[0] == '@') {  // predicated: "@P0 FADD ..."
      const size_t sp = ins.find(' ');
      const size_t b2 = ins.find_first_not_of(' ', sp);
      op = ins.substr(b2, ins.find(' ', b2) - b2);
    }
    const std::string base = op.substr(0, op.find('.'));
    static const char* skip[] = {"LDG", "STG", "LDC", "ULDC", "EXIT", "BRA", "NOP", "RET",
                                 "S2R", "S2UR", "CS2R", "UMOV", "BAR", "LDS", "STS"};
    bool skipped = false;
    for (const char* k : skip) skipped = skipped || base == k;
    if (skipped || op.rfind("IMAD.WIDE", 0) == 0) continue;
    const bool isMov = base == "MOV" || op.rfind("IMAD.MOV", 0) == 0;
    if (isMov && ins.find("c[") != std::string::npos) continue;  // kernel parameter
    if (isMov) ++c.mov;
    else if (base == "MUFU") ++c.mufu;
    else ++c.alu;
    c.sass += ins + "\n";
  }
  c.ok = any;
  if (!any) c.error = "no SASS in nvdisasm output";
  c.cost = c.alu + c.mov + mufuWeight(sm) * c.mufu;
  return c;
}

std::vector<SassCost> measureSass(const std::vector<const Expr*>& exprs,
                                  const std::vector<InputDecl>& inputs, const SassConfig& cfg) {
  std::vector<SassCost> out(exprs.size());
  std::string err;
  const fs::path dir = makeWorkDir(cfg.keepDir, "sopt-sass-", err);
  if (dir.empty()) {
    for (auto& c : out) c.error = err;
    return out;
  }
  const std::string arch = "sm_" + std::to_string(cfg.sm);
  parallelFor(exprs.size(), cfg.threads, [&](size_t i) {
    const fs::path ptx = dir / ("sopt_" + std::to_string(i) + ".ptx");
    const fs::path cubin = dir / ("sopt_" + std::to_string(i) + ".cubin");
    {
      std::ofstream f(ptx, std::ios::binary);
      f << emitPtx(*exprs[i], inputs, cfg.sm);
      if (!f) {
        out[i].error = "cannot write " + ptx.string();
        return;
      }
    }
    int status = 0;
    const std::string log = runCommand(quote(cfg.ptxas) + " -v -O3 -arch=" + arch + " " +
                                           quote(ptx.string()) + " -o " + quote(cubin.string()) +
                                           " 2>&1",
                                       status);
    if (status != 0) {
      out[i].error = "ptxas: " + firstLines(log, 2);
      return;
    }
    const std::string sass =
        runCommand(quote(cfg.nvdisasm) + " -c " + quote(cubin.string()) + " 2>&1", status);
    out[i] = parseSass(sass, cfg.sm);
    if (!out[i].ok && status != 0) out[i].error = "nvdisasm: " + firstLines(sass, 2);
    const size_t used = log.find("Used ");
    if (used != std::string::npos) out[i].regs = std::atoi(log.c_str() + used + 5);
  });
  std::error_code ec;
  if (cfg.keepDir.empty()) fs::remove_all(dir, ec);
  return out;
}

}  // namespace sopt
