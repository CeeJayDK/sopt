#include "measure/driverstats.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "measure/isa.hpp"
#include "measure/tools.hpp"

namespace fs = std::filesystem;

namespace sopt {

std::vector<std::string> exportDriverShaders(const std::vector<const Expr*>& exprsIn,
                                             const std::vector<InputDecl>& inputsIn, const std::string& fxstat,
                                             const fs::path& dir, const std::string& name, std::string& error) {
  const Specialized sp = specializeForCompiler(exprsIn, inputsIn);
  std::vector<std::string> names(sp.exprs.size());
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path work = dir / ".work";
  fs::create_directories(work, ec);
  for (size_t i = 0; i < sp.exprs.size(); ++i) {
    const std::string stem = name + "_" + std::to_string(i);
    const fs::path file = work / (stem + ".fx");
    {
      std::ofstream f(file, std::ios::binary);
      f << emitEffect(*sp.exprs[i], sp.inputs);
    }
    // Unoptimized: the SPIR-V ReShade hands the driver.
    const fs::path out = work / stem;
    int status = 0;
    const std::string log =
        runCommand(quote(fxstat) + " --no-optimize --dump " + quote(out.string()) + " " + quote(file.string()) + " 2>&1", status);
    const fs::path ps = out / "E__SoptPS.spv", vs = out / "E__SoptVS.spv";
    if (!fs::exists(ps)) {
      if (error.empty()) error = "fxstat: " + firstLines(log, 3);
      continue;
    }
    fs::copy_file(ps, dir / (stem + ".ps.spv"), fs::copy_options::overwrite_existing, ec);
    if (fs::exists(vs)) fs::copy_file(vs, dir / (stem + ".vs.spv"), fs::copy_options::overwrite_existing, ec);
    if (!ec) names[i] = stem;
  }
  fs::remove_all(work, ec);
  return names;
}

std::string driverKey(const std::string& file, unsigned line, const std::string& expr) {
  return fs::path(file).filename().string() + ":" + std::to_string(line) + "\t" + expr;
}

namespace {

// One CSV line into fields ("..." may hold commas).
std::vector<std::string> csvFields(const std::string& line) {
  std::vector<std::string> f(1);
  bool quoted = false;
  for (char ch : line) {
    if (ch == '"') quoted = !quoted;
    else if (ch == ',' && !quoted) f.emplace_back();
    else if (ch != '\r') f.back() += ch;
  }
  return f;
}

}  // namespace

bool loadDriverStats(const fs::path& csv, DriverStats& out, std::string& error) {
  std::ifstream in(csv);
  if (!in) {
    error = "cannot read " + csv.string();
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("# vendor: ", 0) == 0) out.vendor = unsigned(std::stoul(line.substr(10), nullptr, 16));
    else if (line.rfind("# gpu: ", 0) == 0) out.gpu = line.substr(7);
    if (line.empty() || line[0] == '#' || line.rfind("shader,", 0) == 0) continue;
    const auto f = csvFields(line);
    if (f.size() < 4) continue;
    if (f[2] == "compiled") {
      out.compiled[f[0]] = f[3] == "yes";
      continue;
    }
    auto& execs = out.shaders[f[0]];
    auto it = std::find_if(execs.begin(), execs.end(), [&](const auto& e) { return e.first == f[1]; });
    if (it == execs.end()) it = execs.insert(execs.end(), {f[1], {}});
    try {
      it->second[f[2]] = std::stod(f[3]);
    } catch (...) {
    }
  }
  std::ifstream man(csv.parent_path() / "manifest.txt");
  if (!man) {
    error = "no manifest.txt next to " + csv.string() + " (sopt-fx --export-spirv writes it)";
    return false;
  }
  while (std::getline(man, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t tab = line.find('\t');
    if (tab != std::string::npos) out.byKey[line.substr(tab + 1)] = line.substr(0, tab);
  }
  return true;
}

int driverCost(const DriverStats& stats, const std::string& shader) {
  const auto it = stats.shaders.find(shader);
  if (it == stats.shaders.end() || stats.vendor != 0x8086) return -1;
  auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const auto& execs = it->second;
  const std::pair<std::string, std::map<std::string, double>>* pick = nullptr;
  for (const auto& e : execs) {
    const std::string n = lower(e.first);
    if (n.find("frag") != std::string::npos || n.find("pixel") != std::string::npos || n.find("ps") == 0) {
      pick = &e;
      break;
    }
  }
  if (!pick && !execs.empty()) pick = &execs.back();  // vertex shader first, pixel shader last
  if (!pick) return -1;
  const auto s = pick->second.find("Instruction Count");
  return s == pick->second.end() ? -1 : int(s->second);
}

}  // namespace sopt
