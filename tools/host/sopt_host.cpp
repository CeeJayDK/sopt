// sopt-host: shows one fixed image with vsync off, so ReShade (with the sopt-timer add-on)
// can be benchmarked without a game (M4 harness). DX11 or Vulkan; Vulkan is loaded at run
// time (vulkan-1.dll), no SDK needed.
//
//   sopt-host [--api dx11|vulkan] [--width 3840] [--height 2160] [--image file.png]
//             [--frames N] [--bench]
//
// --bench sets SOPT_TIMER_AUTO=1 and SOPT_TIMER_EXIT=1: sopt-timer runs its bench over the
// sopt-*.ini presets and closes the window when done. Without --image the input is a
// procedural test image (gradients, colour patches, noise; the same every run).
// There is no depth buffer: depth-based effects see ReShade's empty depth texture.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wincodec.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
  std::string api = "dx11";
  uint32_t width = 3840, height = 2160;
  std::string image;
  uint64_t frames = 0;   // 0 = until closed
  bool bench = false;
};

void fail(const char* what) {
  std::fprintf(stderr, "sopt-host: %s\n", what);
  MessageBoxA(nullptr, what, "sopt-host", MB_ICONERROR);
  std::exit(1);
}

// ---- image -----------------------------------------------------------------------------

uint32_t hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// RGBA8, rows top to bottom.
std::vector<uint8_t> testImage(uint32_t w, uint32_t h) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
      float r, g, b;
      if (v < 0.25f) {   // hue sweep, dark to bright
        const float hue = u * 6.0f, l = v * 4.0f;
        r = std::clamp(std::fabs(hue - 3.0f) - 1.0f, 0.0f, 1.0f) * l;
        g = std::clamp(2.0f - std::fabs(hue - 2.0f), 0.0f, 1.0f) * l;
        b = std::clamp(2.0f - std::fabs(hue - 4.0f), 0.0f, 1.0f) * l;
      } else if (v < 0.5f) {   // grey ramp
        r = g = b = u;
      } else if (v < 0.75f) {   // colour patches with edges
        const uint32_t cell = hash((x / 96) * 7919u + (y / 96));
        r = (cell & 255) / 255.0f;
        g = ((cell >> 8) & 255) / 255.0f;
        b = ((cell >> 16) & 255) / 255.0f;
      } else {   // smooth image-like content plus fine noise
        const float s = 0.5f + 0.25f * std::sin(u * 23.0f) * std::cos(v * 17.0f) + 0.25f * std::sin((u + v) * 61.0f);
        const float n = (hash(y * w + x) & 255) / 255.0f - 0.5f;
        r = s + 0.08f * n;
        g = s * 0.8f + 0.1f + 0.08f * n;
        b = s * 0.6f + 0.2f + 0.08f * n;
      }
      uint8_t* p = &px[(size_t(y) * w + x) * 4];
      p[0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
      p[1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
      p[2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
      p[3] = 255;
    }
  return px;
}

// Loads an image with WIC, scaled (nearest) to w x h, RGBA8.
std::vector<uint8_t> loadImage(const std::string& file, uint32_t w, uint32_t h) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* wic = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
    fail("WIC is not available");
  std::wstring wf(file.begin(), file.end());
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICBitmapSource* rgba = nullptr;
  if (FAILED(wic->CreateDecoderFromFilename(wf.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) ||
      FAILED(dec->GetFrame(0, &frame)) || FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &rgba)))
    fail(("cannot read image " + file).c_str());
  UINT iw = 0, ih = 0;
  rgba->GetSize(&iw, &ih);
  std::vector<uint8_t> src(size_t(iw) * ih * 4);
  rgba->CopyPixels(nullptr, iw * 4, UINT(src.size()), src.data());
  std::vector<uint8_t> px(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const size_t sx = size_t(x) * iw / w, sy = size_t(y) * ih / h;
      std::memcpy(&px[(size_t(y) * w + x) * 4], &src[(sy * iw + sx) * 4], 4);
      px[(size_t(y) * w + x) * 4 + 3] = 255;
    }
  rgba->Release();
  frame->Release();
  dec->Release();
  wic->Release();
  return px;
}

// ---- window ----------------------------------------------------------------------------

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_GETMINMAXINFO: {   // allow a client area larger than the screen (4K on a smaller monitor)
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMaxTrackSize.x = mm->ptMaxTrackSize.y = 32768;
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE && GetKeyState(VK_SHIFT) < 0) DestroyWindow(hwnd);   // plain Esc may belong to ReShade
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND makeWindow(const Options& o) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"sopt-host";
  RegisterClassW(&wc);
  RECT r = {0, 0, LONG(o.width), LONG(o.height)};
  const DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&r, style, FALSE);
  HWND hwnd = CreateWindowW(L"sopt-host", L"sopt-host", style, 0, 0, r.right - r.left, r.bottom - r.top, nullptr,
                            nullptr, wc.hInstance, nullptr);
  if (!hwnd) fail("cannot create window");
  ShowWindow(hwnd, SW_SHOW);
  return hwnd;
}

// Returns false when the window was closed.
bool pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) return false;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return true;
}

void showFps(HWND hwnd, const char* api, uint64_t frame) {
  static ULONGLONG last = GetTickCount64();
  static uint64_t lastFrame = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - last < 1000) return;
  char t[128];
  std::snprintf(t, sizeof(t), "sopt-host (%s) %.1f fps", api, double(frame - lastFrame) * 1000.0 / double(now - last));
  SetWindowTextA(hwnd, t);
  last = now;
  lastFrame = frame;
}

// ---- DX11 ------------------------------------------------------------------------------

int runDx11(const Options& o, HWND hwnd, const std::vector<uint8_t>& px) {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)))
    fail("D3D11CreateDevice failed");
  IDXGIDevice* dxgiDev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* factory = nullptr;
  dev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
  dxgiDev->GetAdapter(&adapter);
  adapter->GetParent(IID_PPV_ARGS(&factory));
  BOOL tearing = FALSE;
  IDXGIFactory5* f5 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
    if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) tearing = FALSE;
    f5->Release();
  }
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = o.width;
  sd.Height = o.height;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 3;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  IDXGISwapChain1* sc = nullptr;
  if (FAILED(factory->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc))) fail("cannot create swap chain");
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = o.width;
  td.Height = o.height;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA init = {px.data(), o.width * 4, 0};
  ID3D11Texture2D* img = nullptr;
  if (FAILED(dev->CreateTexture2D(&td, &init, &img))) fail("cannot create image texture");

  for (uint64_t frame = 0; pump() && (!o.frames || frame < o.frames); ++frame) {
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ctx->CopyResource(bb, img);
    bb->Release();
    sc->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    showFps(hwnd, "dx11", frame);
  }
  ctx->ClearState();
  img->Release();
  sc->Release();
  factory->Release();
  adapter->Release();
  dxgiDev->Release();
  ctx->Release();
  dev->Release();
  return 0;
}

// ---- Vulkan ----------------------------------------------------------------------------

#define VK_FUNCS(X)                                                                       \
  X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)       \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)       \
  X(vkCreateWin32SurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)                       \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)     \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateDevice) X(vkGetDeviceQueue)       \
  X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)              \
  X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers)                  \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)                    \
  X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkQueueSubmit)     \
  X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateSemaphore) X(vkCreateFence)             \
  X(vkWaitForFences) X(vkResetFences) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements)   \
  X(vkBindBufferMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements)                   \
  X(vkBindImageMemory) X(vkAllocateMemory) X(vkMapMemory) X(vkUnmapMemory)                 \
  X(vkDestroyBuffer) X(vkFreeMemory)

#define VK_DECLARE(name) PFN_##name name = nullptr;
VK_FUNCS(VK_DECLARE)
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

void vkCheck(VkResult r, const char* what) {
  if (r != VK_SUCCESS) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed (VkResult %d)", what, int(r));
    fail(buf);
  }
}

uint32_t memoryType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  fail("no suitable Vulkan memory type");
  return 0;
}

void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

int runVulkan(const Options& o, HWND hwnd, const std::vector<uint8_t>& px) {
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (!lib) fail("vulkan-1.dll not found");
  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
  vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "sopt-host";
  app.apiVersion = VK_API_VERSION_1_1;
  const char* instExt[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = 2;
  ici.ppEnabledExtensionNames = instExt;
  VkInstance inst;
  vkCheck(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance");
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(inst, #name));
  VK_FUNCS(VK_LOAD)

  VkWin32SurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  si.hinstance = GetModuleHandleW(nullptr);
  si.hwnd = hwnd;
  VkSurfaceKHR surface;
  vkCheck(vkCreateWin32SurfaceKHR(inst, &si, nullptr, &surface), "vkCreateWin32SurfaceKHR");

  // First discrete GPU with a graphics queue that can present, else the first that can.
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n);
  vkEnumeratePhysicalDevices(inst, &n, pds.data());
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  uint32_t family = 0;
  for (int pass = 0; pass < 2 && !pd; ++pass)
    for (VkPhysicalDevice cand : pds) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(cand, &props);
      if (pass == 0 && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
      uint32_t nf = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(cand, &nf, nullptr);
      std::vector<VkQueueFamilyProperties> fams(nf);
      vkGetPhysicalDeviceQueueFamilyProperties(cand, &nf, fams.data());
      for (uint32_t i = 0; i < nf && !pd; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(cand, i, surface, &present);
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) pd = cand, family = i;
      }
      if (pd) break;
    }
  if (!pd) fail("no Vulkan device can present to the window");

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = devExt;
  VkDevice dev;
  vkCheck(vkCreateDevice(pd, &dci, nullptr, &dev), "vkCreateDevice");
  VkQueue queue;
  vkGetDeviceQueue(dev, family, 0, &queue);

  // Swap chain: B8G8R8A8 or R8G8B8A8 UNORM, immediate > mailbox > fifo.
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
  uint32_t nfmt = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nfmt, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(nfmt);
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nfmt, fmts.data());
  VkSurfaceFormatKHR fmt = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                        : fmts[0];
  for (const auto& f : fmts)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      fmt = f;
      break;
    }
  if (fmt.format != VK_FORMAT_B8G8R8A8_UNORM && fmt.format != VK_FORMAT_R8G8B8A8_UNORM)
    fail("the surface offers no 8-bit UNORM format");
  uint32_t nmodes = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nmodes, nullptr);
  std::vector<VkPresentModeKHR> modes(nmodes);
  vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nmodes, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  for (VkPresentModeKHR want : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
    if (mode == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), want) != modes.end()) mode = want;
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == 0xFFFFFFFFu) extent = {o.width, o.height};
  if (extent.width != o.width || extent.height != o.height)
    std::fprintf(stderr, "sopt-host: window client area is %ux%u, not %ux%u\n", extent.width, extent.height, o.width,
                 o.height);
  VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface;
  sci.minImageCount = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = fmt.format;
  sci.imageColorSpace = fmt.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;
  VkSwapchainKHR sc;
  vkCheck(vkCreateSwapchainKHR(dev, &sci, nullptr, &sc), "vkCreateSwapchainKHR");
  uint32_t nimg = 0;
  vkGetSwapchainImagesKHR(dev, sc, &nimg, nullptr);
  std::vector<VkImage> scImages(nimg);
  vkGetSwapchainImagesKHR(dev, sc, &nimg, scImages.data());

  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = family;
  VkCommandPool pool;
  vkCheck(vkCreateCommandPool(dev, &pci, nullptr, &pool), "vkCreateCommandPool");
  constexpr uint32_t kFrames = 2;
  VkCommandBuffer cbs[kFrames + 1];
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = kFrames + 1;
  vkCheck(vkAllocateCommandBuffers(dev, &cai, cbs), "vkAllocateCommandBuffers");

  // The image, device local, uploaded once (in the swap chain's channel order); copy extent
  // is the swap chain's.
  const uint32_t w = extent.width, h = extent.height;
  VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = fmt.format;
  ii.extent = {w, h, 1};
  ii.mipLevels = ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImage img;
  vkCheck(vkCreateImage(dev, &ii, nullptr, &img), "vkCreateImage");
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, img, &mr);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = memoryType(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory imgMem;
  vkCheck(vkAllocateMemory(dev, &mai, nullptr, &imgMem), "vkAllocateMemory");
  vkBindImageMemory(dev, img, imgMem, 0);

  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(w) * h * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer staging;
  vkCheck(vkCreateBuffer(dev, &bci, nullptr, &staging), "vkCreateBuffer");
  vkGetBufferMemoryRequirements(dev, staging, &mr);
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex =
      memoryType(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory stagingMem;
  vkCheck(vkAllocateMemory(dev, &mai, nullptr, &stagingMem), "vkAllocateMemory");
  vkBindBufferMemory(dev, staging, stagingMem, 0);
  uint8_t* map = nullptr;
  vkMapMemory(dev, stagingMem, 0, bci.size, 0, reinterpret_cast<void**>(&map));
  const bool bgra = fmt.format == VK_FORMAT_B8G8R8A8_UNORM;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const size_t sx = size_t(x) * o.width / w, sy = size_t(y) * o.height / h;
      const uint8_t* s = &px[(sy * o.width + sx) * 4];
      uint8_t* d = &map[(size_t(y) * w + x) * 4];
      d[0] = bgra ? s[2] : s[0];
      d[1] = s[1];
      d[2] = bgra ? s[0] : s[2];
      d[3] = 255;
    }
  vkUnmapMemory(dev, stagingMem);

  VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkCommandBuffer up = cbs[kFrames];
  vkBeginCommandBuffer(up, &cbi);
  imageBarrier(up, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy bic = {};
  bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bic.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(up, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
  imageBarrier(up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkEndCommandBuffer(up);
  VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  sub.commandBufferCount = 1;
  sub.pCommandBuffers = &up;
  vkCheck(vkQueueSubmit(queue, 1, &sub, VK_NULL_HANDLE), "vkQueueSubmit");
  vkQueueWaitIdle(queue);
  vkDestroyBuffer(dev, staging, nullptr);
  vkFreeMemory(dev, stagingMem, nullptr);

  VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkSemaphore acquired[kFrames];
  VkFence fences[kFrames];
  std::vector<VkSemaphore> done(nimg);
  for (uint32_t i = 0; i < kFrames; ++i) {
    vkCreateSemaphore(dev, &semi, nullptr, &acquired[i]);
    vkCreateFence(dev, &fi, nullptr, &fences[i]);
  }
  for (auto& s : done) vkCreateSemaphore(dev, &semi, nullptr, &s);

  for (uint64_t frame = 0; pump() && (!o.frames || frame < o.frames); ++frame) {
    const uint32_t f = uint32_t(frame % kFrames);
    vkWaitForFences(dev, 1, &fences[f], VK_TRUE, UINT64_MAX);
    uint32_t idx = 0;
    const VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired[f], VK_NULL_HANDLE, &idx);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) break;   // window minimized or resized: stop
    vkResetFences(dev, 1, &fences[f]);
    VkCommandBuffer cb = cbs[f];
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &cbi);
    imageBarrier(cb, scImages[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy ic = {};
    ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    ic.extent = {w, h, 1};
    vkCmdCopyImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scImages[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &ic);
    imageBarrier(cb, scImages[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);
    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.waitSemaphoreCount = 1;
    s.pWaitSemaphores = &acquired[f];
    s.pWaitDstStageMask = &wait;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &cb;
    s.signalSemaphoreCount = 1;
    s.pSignalSemaphores = &done[idx];
    vkCheck(vkQueueSubmit(queue, 1, &s, fences[f]), "vkQueueSubmit");
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &idx;
    const VkResult pr = vkQueuePresentKHR(queue, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) break;
    showFps(hwnd, "vulkan", frame);
  }
  vkDeviceWaitIdle(dev);
  return 0;   // the process exits; the driver releases the rest
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) fail(("missing value for " + a).c_str());
      return argv[++i];
    };
    if (a == "--api") o.api = next();
    else if (a == "--width") o.width = uint32_t(std::strtoul(next(), nullptr, 10));
    else if (a == "--height") o.height = uint32_t(std::strtoul(next(), nullptr, 10));
    else if (a == "--image") o.image = next();
    else if (a == "--frames") o.frames = std::strtoull(next(), nullptr, 10);
    else if (a == "--bench") o.bench = true;
    else {
      std::printf("usage: sopt-host [--api dx11|vulkan] [--width 3840] [--height 2160] [--image file] [--frames N] "
                  "[--bench]\n");
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }
  if (o.width == 0 || o.height == 0 || o.width > 16384 || o.height > 16384) fail("bad size");
  if (o.bench) {
    SetEnvironmentVariableA("SOPT_TIMER_AUTO", "1");
    SetEnvironmentVariableA("SOPT_TIMER_EXIT", "1");
  }
  SetProcessDPIAware();   // the client area is in pixels
  const std::vector<uint8_t> px = o.image.empty() ? testImage(o.width, o.height) : loadImage(o.image, o.width, o.height);
  HWND hwnd = makeWindow(o);
  if (o.api == "dx11") return runDx11(o, hwnd, px);
  if (o.api == "vulkan") return runVulkan(o, hwnd, px);
  fail("--api must be dx11 or vulkan");
  return 1;
}
