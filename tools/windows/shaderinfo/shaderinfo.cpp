// ShaderInfo: what the installed GPU drivers report about the shaders they compile (owner, 2026-10-04:
// sopt's NVIDIA counts come from the CUDA compiler, not the graphics driver, and the iand result showed the
// driver version matters). A test of what each driver offers before building on it:
//   - VK_KHR_pipeline_executable_properties: statistics (instruction / register counts, ...) and internal
//     representations (often the disassembly) of a compiled pipeline
//   - VK_AMD_shader_info: AMD's statistics and disassembly
//   - VK_KHR_performance_query: the hardware counters the GPU exposes (listed only)
//   - whether VK_AMD_gpa_interface / VK_INTEL_performance_query exist, and every device extension (in the file)
// Two compute shaders (shaders_spv.h, from int.comp and float.comp) are compiled on every Vulkan GPU;
// everything the driver returns goes to Reports\shaderinfo-<gpu>.txt next to the exe, a summary to the console.
//
//   ShaderInfo [--spv file.spv] [--batch folder] [--all]
//     --spv adds a compute shader of your own (entry point main); --batch compiles every *.ps.spv in the folder
//     as a pixel shader pipeline and writes the driver's statistics to shaderinfo-batch-<gpu>.csv there;
//     --all includes software renderers
//
// Vulkan is loaded at run time (vulkan-1.dll), no SDK needed.

#include "../benchkit.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <fstream>
#include <string>
#include <vector>

#include "shaders_spv.h"

using namespace benchkit;

namespace {

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VK_INSTANCE_FUNCS(X)                              \
  X(vkEnumeratePhysicalDevices)                           \
  X(vkGetPhysicalDeviceProperties)                        \
  X(vkGetPhysicalDeviceProperties2)                       \
  X(vkGetPhysicalDeviceFeatures2)                         \
  X(vkGetPhysicalDeviceQueueFamilyProperties)             \
  X(vkEnumerateDeviceExtensionProperties)                 \
  X(vkCreateDevice)                                       \
  X(vkGetDeviceProcAddr)                                  \
  X(vkDestroyInstance)
#define VK_DEVICE_FUNCS(X)          \
  X(vkDestroyDevice)                \
  X(vkCreateShaderModule)           \
  X(vkDestroyShaderModule)          \
  X(vkCreateDescriptorSetLayout)    \
  X(vkDestroyDescriptorSetLayout)   \
  X(vkCreatePipelineLayout)         \
  X(vkDestroyPipelineLayout)        \
  X(vkCreateComputePipelines)       \
  X(vkCreateGraphicsPipelines)      \
  X(vkCreateRenderPass)             \
  X(vkDestroyRenderPass)            \
  X(vkDestroyPipeline)
#define VK_DECLARE(name) PFN_##name name;
VK_INSTANCE_FUNCS(VK_DECLARE)
VK_DEVICE_FUNCS(VK_DECLARE)
PFN_vkGetPipelineExecutablePropertiesKHR vkGetPipelineExecutablePropertiesKHR;
PFN_vkGetPipelineExecutableStatisticsKHR vkGetPipelineExecutableStatisticsKHR;
PFN_vkGetPipelineExecutableInternalRepresentationsKHR vkGetPipelineExecutableInternalRepresentationsKHR;
PFN_vkGetShaderInfoAMD vkGetShaderInfoAMD;
PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR;

struct Shader {
  std::string name;
  std::vector<uint32_t> code;
};

std::string statValue(const VkPipelineExecutableStatisticKHR& s) {
  switch (s.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: return s.value.b32 ? "true" : "false";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: return std::to_string(s.value.i64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: return std::to_string(s.value.u64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: return std::to_string(s.value.f64);
    default: return "?";
  }
}

// The driver version as the vendor writes it (NVIDIA 10.8.8.6 bits, Intel on Windows 18.14 bits).
std::string driverVersion(const VkPhysicalDeviceProperties& p) {
  const uint32_t v = p.driverVersion;
  char buf[64];
  if (p.vendorID == 0x10DE) std::snprintf(buf, sizeof(buf), "%u.%u", v >> 22, (v >> 14) & 0xff);
  else if (p.vendorID == 0x8086) std::snprintf(buf, sizeof(buf), "%u.%u", v >> 14, v & 0x3fff);
  else std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
  return buf;
}

// --batch: the descriptor bindings a SPIR-V module declares (set, binding, type), read from its decorations,
// so the pipeline layout matches the shader (fxstat's ReShade modules: a combined image sampler per texture in
// set 1, the uniform buffer in set 0).
struct Binding {
  uint32_t set = 0, binding = 0;
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
};

std::vector<Binding> reflectBindings(const std::vector<uint32_t>& code) {
  std::map<uint32_t, uint32_t> set, binding, pointee, storage, typeOp, elem, imageSampled, varType;
  std::map<uint32_t, bool> bufferBlock;
  std::vector<uint32_t> vars;
  for (size_t i = 5; i < code.size();) {
    const uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
    if (len == 0 || i + len > code.size()) break;
    const uint32_t* w = &code[i];
    if (op == 71 && len >= 4 && w[2] == 34) set[w[1]] = w[3];             // OpDecorate DescriptorSet
    else if (op == 71 && len >= 4 && w[2] == 33) binding[w[1]] = w[3];    // OpDecorate Binding
    else if (op == 71 && len >= 3 && w[2] == 3) bufferBlock[w[1]] = true; // OpDecorate BufferBlock
    else if (op == 32 && len >= 4) pointee[w[1]] = w[3];                  // OpTypePointer
    else if (op == 25 && len >= 8) typeOp[w[1]] = op, imageSampled[w[1]] = w[7];  // OpTypeImage
    else if (op == 26 || op == 27 || op == 30) typeOp[w[1]] = op;         // sampler, sampled image, struct
    else if ((op == 28 || op == 29) && len >= 3) typeOp[w[1]] = op, elem[w[1]] = w[2];  // (runtime) array
    else if (op == 59 && len >= 4) vars.push_back(w[2]), varType[w[2]] = w[1], storage[w[2]] = w[3];  // OpVariable
    i += len;
  }
  std::vector<Binding> out;
  for (uint32_t v : vars) {
    if (!set.count(v) || !binding.count(v)) continue;
    uint32_t t = pointee[varType[v]];
    while (typeOp.count(t) && (typeOp[t] == 28 || typeOp[t] == 29)) t = elem[t];  // arrays of resources
    const uint32_t top = typeOp.count(t) ? typeOp[t] : 0, sc = storage[v];
    Binding b;
    b.set = set[v];
    b.binding = binding[v];
    if (sc == 12) b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    else if (sc == 2) b.type = bufferBlock.count(t) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    else if (top == 27) b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    else if (top == 26) b.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    else if (top == 25) b.type = imageSampled[t] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    else continue;
    out.push_back(b);
  }
  return out;
}

// The first entry point of the given execution model (0 vertex, 4 fragment, 5 compute), or "".
std::string entryPoint(const std::vector<uint32_t>& code, uint32_t model) {
  for (size_t i = 5; i < code.size();) {
    const uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
    if (len == 0 || i + len > code.size()) break;
    if (op == 15 && len >= 4 && code[i + 1] == model) return std::string(reinterpret_cast<const char*>(&code[i + 3]));
    i += len;
  }
  return "";
}

std::vector<uint32_t> readSpv(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<uint32_t> code(bytes.size() / 4);
  if (bytes.size() >= 20 && bytes.size() % 4 == 0) std::memcpy(code.data(), bytes.data(), bytes.size());
  else code.clear();
  return code;
}

// --batch DIR: every *.ps.spv in DIR (a pixel shader module, with NAME.vs.spv as its vertex shader when present,
// else a full-screen triangle) compiled as a graphics pipeline; every statistic the driver reports goes to csv as
// shader,executable,statistic,value (owner, 2026-10-04: the Intel driver's instruction / cycle counts for sopt's
// variants; sopt-fx --export-spirv writes the folder, --driver-stats reads the file back).
size_t runBatch(VkDevice dev, bool pepOn, const std::filesystem::path& dir, FILE* csv) {
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    const std::string n = e.path().filename().string();
    if (n.size() > 7 && n.compare(n.size() - 7, 7, ".ps.spv") == 0) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  VkRenderPass pass;
  if (vkCreateRenderPass(dev, &rpi, nullptr, &pass) != VK_SUCCESS) return 0;
  const std::vector<uint32_t> fallbackVs(std::begin(kFullscreenVsSpv), std::end(kFullscreenVsSpv));
  size_t compiled = 0;
  for (const auto& file : files) {
    const std::string name = file.filename().string().substr(0, file.filename().string().size() - 7);
    const std::vector<uint32_t> ps = readSpv(file);
    const std::filesystem::path vsFile = file.parent_path() / (name + ".vs.spv");
    const std::vector<uint32_t> vs = std::filesystem::exists(vsFile) ? readSpv(vsFile) : fallbackVs;
    const std::string psEntry = entryPoint(ps, 4), vsEntry = entryPoint(vs, 0);
    bool ok = !ps.empty() && !vs.empty() && !psEntry.empty() && !vsEntry.empty();
    VkShaderModule mods[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::vector<VkDescriptorSetLayout> sets;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (ok) {
      for (int k = 0; k < 2 && ok; ++k) {
        const std::vector<uint32_t>& c = k ? ps : vs;
        VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = c.size() * 4;
        smi.pCode = c.data();
        ok = vkCreateShaderModule(dev, &smi, nullptr, &mods[k]) == VK_SUCCESS;
      }
    }
    if (ok) {
      // The pipeline layout from both modules' bindings (sets 0 .. the highest used).
      std::vector<Binding> bs = reflectBindings(ps);
      for (const Binding& b : reflectBindings(vs)) bs.push_back(b);
      uint32_t maxSet = 0;
      for (const Binding& b : bs) maxSet = std::max(maxSet, b.set);
      for (uint32_t si = 0; si <= maxSet && ok; ++si) {
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const Binding& b : bs) {
          if (b.set != si) continue;
          bool dup = false;
          for (const auto& x : lb) dup |= x.binding == b.binding;
          if (!dup) lb.push_back({b.binding, b.type, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = uint32_t(lb.size());
        dl.pBindings = lb.data();
        VkDescriptorSetLayout sl;
        ok = vkCreateDescriptorSetLayout(dev, &dl, nullptr, &sl) == VK_SUCCESS;
        if (ok) sets.push_back(sl);
      }
      VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl.setLayoutCount = uint32_t(sets.size());
      pl.pSetLayouts = sets.data();
      ok = ok && vkCreatePipelineLayout(dev, &pl, nullptr, &layout) == VK_SUCCESS;
    }
    if (ok) {
      VkPipelineShaderStageCreateInfo stages[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mods[0], vsEntry.c_str(), nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mods[1], psEntry.c_str(), nullptr}};
      VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vp.viewportCount = vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.cullMode = VK_CULL_MODE_NONE;
      rs.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState cba = {};
      cba.colorWriteMask = 0xf;
      VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      cb.attachmentCount = 1;
      cb.pAttachments = &cba;
      const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      ds.dynamicStateCount = 2;
      ds.pDynamicStates = dyn;
      VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      if (pepOn) gpi.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
      gpi.stageCount = 2;
      gpi.pStages = stages;
      gpi.pVertexInputState = &vi;
      gpi.pInputAssemblyState = &ia;
      gpi.pViewportState = &vp;
      gpi.pRasterizationState = &rs;
      gpi.pMultisampleState = &ms;
      gpi.pColorBlendState = &cb;
      gpi.pDynamicState = &ds;
      gpi.layout = layout;
      gpi.renderPass = pass;
      ok = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe) == VK_SUCCESS;
    }
    std::fprintf(csv, "%s,,compiled,%s\n", name.c_str(), ok ? "yes" : "no");
    if (ok) {
      ++compiled;
      if (pepOn) {
        VkPipelineInfoKHR pi = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
        pi.pipeline = pipe;
        uint32_t nx = 0;
        vkGetPipelineExecutablePropertiesKHR(dev, &pi, &nx, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> xs(nx, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        vkGetPipelineExecutablePropertiesKHR(dev, &pi, &nx, xs.data());
        for (uint32_t e = 0; e < nx; ++e) {
          VkPipelineExecutableInfoKHR ei = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
          ei.pipeline = pipe;
          ei.executableIndex = e;
          uint32_t ns = 0;
          vkGetPipelineExecutableStatisticsKHR(dev, &ei, &ns, nullptr);
          std::vector<VkPipelineExecutableStatisticKHR> stats(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
          vkGetPipelineExecutableStatisticsKHR(dev, &ei, &ns, stats.data());
          for (const auto& st : stats)
            std::fprintf(csv, "%s,\"%s\",\"%s\",%s\n", name.c_str(), xs[e].name, st.name, statValue(st).c_str());
        }
      }
      vkDestroyPipeline(dev, pipe, nullptr);
    }
    if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
    for (auto sl : sets) vkDestroyDescriptorSetLayout(dev, sl, nullptr);
    for (auto m : mods)
      if (m) vkDestroyShaderModule(dev, m, nullptr);
  }
  vkDestroyRenderPass(dev, pass, nullptr);
  return compiled;
}

}  // namespace

int main(int argc, char** argv) {
  gProgram = "ShaderInfo";
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const Style st = initConsole();
  std::vector<Shader> shaders = {{"int (and / xor / add / mul chain)", std::vector<uint32_t>(std::begin(kIntSpv), std::end(kIntSpv))},
                                 {"float (fma, rcp, sqrt, floor, max, clamp, sign, exp2)",
                                  std::vector<uint32_t>(std::begin(kFloatSpv), std::end(kFloatSpv))}};
  bool all = false;
  std::filesystem::path batch;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--all") {
      all = true;
    } else if (a == "--batch" && i + 1 < argc) {
      batch = argv[++i];
      if (!std::filesystem::is_directory(batch)) fail("not a folder: " + batch.string());
    } else if (a == "--spv" && i + 1 < argc) {
      std::ifstream f(argv[++i], std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (bytes.size() < 20 || bytes.size() % 4) fail(std::string("not a SPIR-V file: ") + argv[i]);
      Shader s{argv[i], std::vector<uint32_t>(bytes.size() / 4)};
      std::memcpy(s.code.data(), bytes.data(), bytes.size());
      shaders.push_back(std::move(s));
    } else {
      std::printf("ShaderInfo %s\nusage: ShaderInfo [--spv file.spv] [--batch folder] [--all]\n", SOPT_VERSION);
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }
  printBox(st, std::string("ShaderInfo ") + SOPT_VERSION + "  -  by CeeJay.dk");

  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (!lib) fail("vulkan-1.dll not found (no Vulkan driver installed?)");
  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
  auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "ShaderInfo";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance inst;
  if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) fail("vkCreateInstance failed");
#define VK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(inst, #name));
  VK_INSTANCE_FUNCS(VK_LOAD_INSTANCE)
  vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR =
      reinterpret_cast<PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR>(
          vkGetInstanceProcAddr(inst, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR"));

  const std::filesystem::path here = reportsDir();

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n);
  vkEnumeratePhysicalDevices(inst, &n, pds.data());
  if (pds.empty()) fail("no Vulkan GPU found");
  std::vector<std::string> files;
  for (VkPhysicalDevice pd : pds) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !all) continue;  // software renderers
    std::string name = props.deviceName, safe = name;
    for (char& ch : safe)
      if (!isalnum(static_cast<unsigned char>(ch))) ch = '_';
    std::string report;
    auto out = [&](const std::string& s) { report += s; };
    // Driver name and version string (VK_KHR_driver_properties, core in 1.2) where available.
    std::string driverInfo = driverVersion(props);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
    auto has = [&](const char* e) {
      for (const auto& x : exts)
        if (std::strcmp(x.extensionName, e) == 0) return true;
      return false;
    };
    if (has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
      VkPhysicalDeviceDriverPropertiesKHR dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR};
      VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
      vkGetPhysicalDeviceProperties2(pd, &p2);
      driverInfo = std::string(dp.driverName) + " " + dp.driverInfo + " (" + driverVersion(props) + ")";
    }
    const bool pep = has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME), amd = has(VK_AMD_SHADER_INFO_EXTENSION_NAME),
               perf = has(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);
    // Vendor profiling interfaces (listed only): AMD's GPA (counters, thread traces, a stable profiling clock
    // mode; newer than the vendored headers) and Intel's performance query.
    const bool gpa = has("VK_AMD_gpa_interface"), intelPerf = has(VK_INTEL_PERFORMANCE_QUERY_EXTENSION_NAME);
    std::printf("\nGPU: %s%s%s (vendor 0x%04X, device 0x%04X)\n  driver %s\n", vendorColor(st, props.vendorID), name.c_str(),
                st.reset(), props.vendorID, props.deviceID, driverInfo.c_str());
    out("GPU: " + name + "\nvendor " + std::to_string(props.vendorID) + ", device " + std::to_string(props.deviceID) +
        "\ndriver: " + driverInfo + "\n\n");
    auto yesNo = [&](bool b) { return b ? std::string(st.c("\x1b[92m")) + "yes" + st.reset() : std::string(st.c("\x1b[91m")) + "no" + st.reset(); };
    std::printf("  pipeline executable properties: %s, AMD shader info: %s, performance query: %s\n"
                "  AMD GPA interface: %s, Intel performance query: %s\n",
                yesNo(pep).c_str(), yesNo(amd).c_str(), yesNo(perf).c_str(), yesNo(gpa).c_str(), yesNo(intelPerf).c_str());
    out(std::string("VK_KHR_pipeline_executable_properties: ") + (pep ? "yes" : "no") + "\nVK_AMD_shader_info: " +
        (amd ? "yes" : "no") + "\nVK_KHR_performance_query: " + (perf ? "yes" : "no") + "\nVK_AMD_gpa_interface: " +
        (gpa ? "yes" : "no") + "\nVK_INTEL_performance_query: " + (intelPerf ? "yes" : "no") + "\n");
    out("\nAll device extensions (" + std::to_string(exts.size()) + "):\n");
    for (const auto& x : exts) out("  " + std::string(x.extensionName) + " " + std::to_string(x.specVersion) + "\n");

    // A compute queue family.
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nf, fams.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nf && family == UINT32_MAX; ++i)
      if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) family = i;

    // Hardware counters (listed, not read).
    if (perf && vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR && family != UINT32_MAX) {
      uint32_t nc = 0;
      vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(pd, family, &nc, nullptr, nullptr);
      std::vector<VkPerformanceCounterKHR> counters(nc, {VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR});
      std::vector<VkPerformanceCounterDescriptionKHR> descs(nc, {VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR});
      vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(pd, family, &nc, counters.data(), descs.data());
      std::printf("  hardware counters: %u\n", nc);
      out("\nHardware counters (" + std::to_string(nc) + "):\n");
      for (uint32_t i = 0; i < nc; ++i)
        out("  " + std::string(descs[i].category) + " / " + descs[i].name + ": " + descs[i].description + "\n");
    }

    if (!pep && !amd && batch.empty()) {
      std::printf("  this driver reports nothing about compiled shaders\n");
    } else if (family == UINT32_MAX) {
      std::printf("  no compute queue\n");
    } else {
      // A device with the extensions (and pipelineExecutableInfo enabled).
      VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pf = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
      VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pf};
      vkGetPhysicalDeviceFeatures2(pd, &f2);
      const bool pepOn = pep && pf.pipelineExecutableInfo;
      pf.pNext = nullptr;
      f2.features = {};
      std::vector<const char*> enable;
      if (pepOn) enable.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
      if (amd) enable.push_back(VK_AMD_SHADER_INFO_EXTENSION_NAME);
      const float prio = 1.0f;
      VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      qci.queueFamilyIndex = family;
      qci.queueCount = 1;
      qci.pQueuePriorities = &prio;
      VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      dci.pNext = pepOn ? &f2 : nullptr;
      dci.queueCreateInfoCount = 1;
      dci.pQueueCreateInfos = &qci;
      dci.enabledExtensionCount = uint32_t(enable.size());
      dci.ppEnabledExtensionNames = enable.data();
      VkDevice dev;
      if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) {
        std::printf("  vkCreateDevice failed\n");
        continue;
      }
#define VK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev, #name));
      VK_DEVICE_FUNCS(VK_LOAD_DEVICE)
      VK_LOAD_DEVICE(vkGetPipelineExecutablePropertiesKHR)
      VK_LOAD_DEVICE(vkGetPipelineExecutableStatisticsKHR)
      VK_LOAD_DEVICE(vkGetPipelineExecutableInternalRepresentationsKHR)
      VK_LOAD_DEVICE(vkGetShaderInfoAMD)

      VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      dl.bindingCount = 1;
      dl.pBindings = &binding;
      VkDescriptorSetLayout setLayout;
      vkCreateDescriptorSetLayout(dev, &dl, nullptr, &setLayout);
      VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
      VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl.setLayoutCount = 1;
      pl.pSetLayouts = &setLayout;
      pl.pushConstantRangeCount = 1;
      pl.pPushConstantRanges = &pc;
      VkPipelineLayout layout;
      vkCreatePipelineLayout(dev, &pl, nullptr, &layout);

      if (!batch.empty()) {
        const std::filesystem::path csvPath = batch / ("shaderinfo-batch-" + safe + ".csv");
        if (FILE* csv = std::fopen(csvPath.string().c_str(), "wb")) {
          std::fprintf(csv, "# ShaderInfo %s\n# gpu: %s\n# vendor: 0x%04X\n# device: 0x%04X\n# driver: %s\nshader,executable,statistic,value\n",
                       SOPT_VERSION, name.c_str(), props.vendorID, props.deviceID, driverInfo.c_str());
          const size_t n = runBatch(dev, pepOn, batch, csv);
          std::fclose(csv);
          std::printf("  batch: %zu shaders compiled -> %s\n", n, csvPath.string().c_str());
          files.push_back(csvPath.string());
        }
      }
      for (const Shader& sh : shaders) {
        if (!batch.empty()) break;  // --batch: only the folder's shaders
        out("\n==== " + sh.name + " ====\n");
        std::printf("  %s%s%s\n", st.c("\x1b[1;96m"), sh.name.c_str(), st.reset());
        VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = sh.code.size() * 4;
        smi.pCode = sh.code.data();
        VkShaderModule mod;
        if (vkCreateShaderModule(dev, &smi, nullptr, &mod) != VK_SUCCESS) {
          std::printf("    vkCreateShaderModule failed\n");
          continue;
        }
        VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        if (pepOn)
          cpi.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        cpi.layout = layout;
        VkPipeline pipe;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe) != VK_SUCCESS) {
          std::printf("    vkCreateComputePipelines failed\n");
          vkDestroyShaderModule(dev, mod, nullptr);
          continue;
        }
        if (pepOn) {
          VkPipelineInfoKHR pi = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
          pi.pipeline = pipe;
          uint32_t nx = 0;
          vkGetPipelineExecutablePropertiesKHR(dev, &pi, &nx, nullptr);
          std::vector<VkPipelineExecutablePropertiesKHR> xs(nx, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
          vkGetPipelineExecutablePropertiesKHR(dev, &pi, &nx, xs.data());
          for (uint32_t e = 0; e < nx; ++e) {
            out("-- executable " + std::to_string(e) + ": " + xs[e].name + " (" + xs[e].description + "), subgroup size " +
                std::to_string(xs[e].subgroupSize) + "\n");
            std::printf("    executable %s, subgroup size %u\n", xs[e].name, xs[e].subgroupSize);
            VkPipelineExecutableInfoKHR ei = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            ei.pipeline = pipe;
            ei.executableIndex = e;
            uint32_t ns = 0;
            vkGetPipelineExecutableStatisticsKHR(dev, &ei, &ns, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> stats(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            vkGetPipelineExecutableStatisticsKHR(dev, &ei, &ns, stats.data());
            for (const auto& s : stats) {
              out("  " + std::string(s.name) + " = " + statValue(s) + "   (" + s.description + ")\n");
              std::printf("      %-32s %s\n", s.name, statValue(s).c_str());
            }
            uint32_t nr = 0;
            vkGetPipelineExecutableInternalRepresentationsKHR(dev, &ei, &nr, nullptr);
            std::vector<VkPipelineExecutableInternalRepresentationKHR> reps(
                nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            vkGetPipelineExecutableInternalRepresentationsKHR(dev, &ei, &nr, reps.data());  // sizes
            std::vector<std::vector<char>> data(nr);
            for (uint32_t r = 0; r < nr; ++r) {
              data[r].resize(reps[r].dataSize + 1, 0);
              reps[r].pData = data[r].data();
            }
            if (nr) vkGetPipelineExecutableInternalRepresentationsKHR(dev, &ei, &nr, reps.data());
            for (uint32_t r = 0; r < nr; ++r) {
              std::printf("      representation: %s (%s, %zu bytes)\n", reps[r].name, reps[r].isText ? "text" : "binary",
                          size_t(reps[r].dataSize));
              out("  -- representation " + std::string(reps[r].name) + " (" + reps[r].description + ")\n");
              if (reps[r].isText) out(std::string(data[r].data()) + "\n");
              else out("  (binary, " + std::to_string(reps[r].dataSize) + " bytes)\n");
            }
            if (nr == 0) std::printf("      no internal representations (no disassembly)\n");
          }
        }
        if (amd && vkGetShaderInfoAMD) {
          VkShaderStatisticsInfoAMD ss = {};
          size_t size = sizeof(ss);
          if (vkGetShaderInfoAMD(dev, pipe, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_INFO_TYPE_STATISTICS_AMD, &size, &ss) == VK_SUCCESS) {
            std::printf("    AMD: %u VGPRs, %u SGPRs used\n", ss.resourceUsage.numUsedVgprs, ss.resourceUsage.numUsedSgprs);
            out("AMD statistics: VGPRs " + std::to_string(ss.resourceUsage.numUsedVgprs) + ", SGPRs " +
                std::to_string(ss.resourceUsage.numUsedSgprs) + "\n");
          }
          size = 0;
          if (vkGetShaderInfoAMD(dev, pipe, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, &size, nullptr) ==
                  VK_SUCCESS &&
              size) {
            std::vector<char> text(size + 1, 0);
            vkGetShaderInfoAMD(dev, pipe, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, &size, text.data());
            std::printf("    AMD disassembly: %zu bytes\n", size);
            out("-- AMD disassembly\n" + std::string(text.data()) + "\n");
          }
        }
        vkDestroyPipeline(dev, pipe, nullptr);
        vkDestroyShaderModule(dev, mod, nullptr);
      }
      vkDestroyPipelineLayout(dev, layout, nullptr);
      vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
      vkDestroyDevice(dev, nullptr);
    }
    const std::filesystem::path file = here / ("shaderinfo-" + safe + ".txt");
    if (FILE* f = std::fopen(file.string().c_str(), "wb")) {
      std::fwrite(report.data(), 1, report.size(), f);
      std::fclose(f);
      files.push_back(file.string());
    }
  }
  vkDestroyInstance(inst, nullptr);
  std::printf("\n");
  for (const std::string& f : files) std::printf("  Report: %s\n", f.c_str());
  std::printf("  Please send these files; they show what each driver tells about the shaders it compiles.\n");
  gCurrent.clear();
  setTitle("done");
  return 0;
}
