#include "measure/backends.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "measure/isa.hpp"
#include "measure/tools.hpp"

namespace sopt {
namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// "alu" of the pixel entry point in fxstat --json (the optimized counts come first).
int pixelAlu(const std::string& json) {
  const size_t stage = json.find("\"stage\": \"pixel\"");
  if (stage == std::string::npos) return -1;
  const size_t k = json.find("\"alu\":", stage);
  if (k == std::string::npos) return -1;
  return std::atoi(json.c_str() + k + 6);
}

// A path as Wine's Windows side sees it (Z: is the Unix root).
std::string winePath(const fs::path& p) {
  std::string s = "Z:" + fs::absolute(p).string();
  for (char& c : s)
    if (c == '/') c = '\\';
  return s;
}

}  // namespace

std::string dxbcInstructions(const std::string& disassembly, int& count) {
  std::istringstream in(disassembly);
  std::string line, out;
  count = 0;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t b = line.find_first_not_of(' ');
    if (b == std::string::npos) continue;
    line = line.substr(b);
    if (line.rfind("//", 0) == 0 || line.rfind("dcl_", 0) == 0 || line == "ret") continue;
    if ((line[0] == 'p' || line[0] == 'v') && line.size() >= 6 && line[1] == 's' && line[2] == '_') continue;  // ps_5_0
    out += line + "\n";
    ++count;
  }
  return out;
}

std::string canonicalSpirv(const std::string& text) {
  std::istringstream in(text);
  std::string line, out;
  std::unordered_map<std::string, std::string> ids;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == ';') continue;
    if (line.find("OpName") != std::string::npos || line.find("OpMemberName") != std::string::npos ||
        line.find("OpSource") != std::string::npos || line.find("OpString") != std::string::npos ||
        line.find("OpLine") != std::string::npos || line.find("OpModuleProcessed") != std::string::npos)
      continue;
    std::string res;
    for (size_t i = 0; i < line.size();) {
      if (line[i] == '%') {
        size_t j = i + 1;
        while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
        const std::string id = line.substr(i, j - i);
        auto it = ids.find(id);
        if (it == ids.end()) it = ids.emplace(id, "%" + std::to_string(ids.size() + 1)).first;
        res += it->second;
        i = j;
      } else {
        res += line[i++];
      }
    }
    const size_t b = res.find_first_not_of(' ');
    out += (b == std::string::npos ? res : res.substr(b)) + "\n";
  }
  return out;
}

std::vector<BackendCode> measureBackends(const std::vector<const Expr*>& exprsIn,
                                         const std::vector<InputDecl>& inputsIn, const BackendConfig& cfg) {
  const Specialized sp = specializeForCompiler(exprsIn, inputsIn);
  std::vector<BackendCode> out(sp.exprs.size());
  if (sp.exprs.empty()) return out;
  std::string err;
  const fs::path dir = makeWorkDir(cfg.keepDir, "sopt-backends-", err);
  if (dir.empty()) {
    for (auto& c : out) c.error = err;
    return out;
  }
  parallelFor(sp.exprs.size(), cfg.threads, [&](size_t i) {
    BackendCode& c = out[i];
    const std::string stem = "sopt_" + std::to_string(i);
    const fs::path file = dir / (stem + ".fx");
    {
      std::ofstream f(file, std::ios::binary);
      f << emitEffect(*sp.exprs[i], sp.inputs);
      if (!f) {
        c.error = "cannot write " + file.string();
        return;
      }
    }
    int status = 0;
    // SPIR-V: optimized counts, and the optimized module for the identity check.
    const fs::path spv = dir / (stem + "_spv");
    const std::string json =
        runCommand(quote(cfg.fxstat) + " --json --dump " + quote(spv.string()) + " " + quote(file.string()) + " 2>&1", status);
    c.spirv = pixelAlu(json);
    if (c.spirv < 0) c.error = "spirv: " + firstLines(json, 3);
    if (!cfg.spirvDis.empty() && fs::exists(spv / "E__SoptPS.spv"))
      c.spirvCode = canonicalSpirv(
          runCommand(quote(cfg.spirvDis) + " --raw-id " + quote((spv / "E__SoptPS.spv").string()) + " 2>&1", status));
    // DXBC: ReShade's HLSL through Microsoft's fxc at -O3.
    if (!cfg.fxc.empty()) {
      const fs::path hl = dir / (stem + "_hlsl");
      runCommand(quote(cfg.fxstat) + " --hlsl --dump " + quote(hl.string()) + " " + quote(file.string()) + " 2>&1", status);
      const fs::path src = hl / (stem + ".hlsl");
      if (!fs::exists(src)) {
        if (c.error.empty()) c.error = "dxbc: fxstat wrote no HLSL";
        return;
      }
      std::string cmd;
      if (!cfg.wine.empty())
        cmd = "WINEDEBUG=-all WINEDLLOVERRIDES=d3dcompiler_47=n " + cfg.wine + " " + quote(cfg.fxc) + " " +
              quote(winePath(src));
      else
        cmd = quote(cfg.fxc) + " " + quote(src.string());
      const std::string dis = runCommand(cmd + " F__SoptPS ps_5_0 2>&1", status);
      int n = 0;
      c.dxbcCode = dxbcInstructions(dis, n);
      if (status == 0 && dis.find("ps_5_0") != std::string::npos) c.dxbc = n;
      else if (c.error.empty()) c.error = "dxbc: " + firstLines(dis, 3);
    }
  });
  std::error_code ec;
  if (cfg.keepDir.empty()) fs::remove_all(dir, ec);
  return out;
}

}  // namespace sopt
