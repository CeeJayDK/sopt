// ShaderInfo: what the installed GPU drivers report about the shaders they compile (owner, 2026-10-04:
// sopt's NVIDIA counts come from the CUDA compiler, not the graphics driver, and the iand result showed the
// driver version matters). A test of what each driver offers before building on it:
//   - VK_KHR_pipeline_executable_properties: statistics (instruction / register counts, ...) and internal
//     representations (often the disassembly) of a compiled pipeline
//   - VK_AMD_shader_info: AMD's statistics and disassembly
//   - VK_KHR_performance_query: the hardware counters the GPU exposes (listed only)
// Two compute shaders (shaders_spv.h, from int.comp and float.comp) are compiled on every Vulkan GPU;
// everything the driver returns goes to shaderinfo-<gpu>.txt next to the exe, a summary to the console.
//
//   ShaderInfo [--spv file.spv] [--all]   (--spv adds a compute shader of your own, entry point main;
//                                        --all includes software renderers)
//
// Vulkan is loaded at run time (vulkan-1.dll), no SDK needed.

#include "../benchkit.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
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

}  // namespace

int main(int argc, char** argv) {
  gProgram = "ShaderInfo";
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const Style st = initConsole();
  std::vector<Shader> shaders = {{"int (and / xor / add / mul chain)", std::vector<uint32_t>(std::begin(kIntSpv), std::end(kIntSpv))},
                                 {"float (fma, rcp, sqrt, floor, max, clamp, sign, exp2)",
                                  std::vector<uint32_t>(std::begin(kFloatSpv), std::end(kFloatSpv))}};
  bool all = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--all") {
      all = true;
    } else if (a == "--spv" && i + 1 < argc) {
      std::ifstream f(argv[++i], std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (bytes.size() < 20 || bytes.size() % 4) fail(std::string("not a SPIR-V file: ") + argv[i]);
      Shader s{argv[i], std::vector<uint32_t>(bytes.size() / 4)};
      std::memcpy(s.code.data(), bytes.data(), bytes.size());
      shaders.push_back(std::move(s));
    } else {
      std::printf("ShaderInfo %s\nusage: ShaderInfo [--spv file.spv] [--all]\n", SOPT_VERSION);
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

  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  const std::filesystem::path here = std::filesystem::path(exePath).parent_path();

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
    std::printf("\nGPU: %s%s%s (vendor 0x%04X, device 0x%04X)\n  driver %s\n", vendorColor(st, props.vendorID), name.c_str(),
                st.reset(), props.vendorID, props.deviceID, driverInfo.c_str());
    out("GPU: " + name + "\nvendor " + std::to_string(props.vendorID) + ", device " + std::to_string(props.deviceID) +
        "\ndriver: " + driverInfo + "\n\n");
    auto yesNo = [&](bool b) { return b ? std::string(st.c("\x1b[92m")) + "yes" + st.reset() : std::string(st.c("\x1b[91m")) + "no" + st.reset(); };
    std::printf("  pipeline executable properties: %s, AMD shader info: %s, performance query: %s\n", yesNo(pep).c_str(),
                yesNo(amd).c_str(), yesNo(perf).c_str());
    out(std::string("VK_KHR_pipeline_executable_properties: ") + (pep ? "yes" : "no") + "\nVK_AMD_shader_info: " +
        (amd ? "yes" : "no") + "\nVK_KHR_performance_query: " + (perf ? "yes" : "no") + "\n");

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

    if (!pep && !amd) {
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

      for (const Shader& sh : shaders) {
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
  return 0;
}
