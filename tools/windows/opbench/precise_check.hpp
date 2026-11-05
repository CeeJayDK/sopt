// Does `precise` hold? (owner, 2026-11-03: (x + c) - c came out cancelled even as precise on some card and API pairs:
// GTX 1660 on D3D9 and D3D12, UHD 630 on D3D10, without ReShade too.) Each API draws the forms OpBench times and a float2
// written in one line, as a shader would, next to the real round / floor / frac, and reads the difference back. The result
// depends on the form (D3D12 on the 1660 cancels only the float2 one), so "right" does not prove another form safe. A
// cancelled form also means its roundadd / flooradd / fracadd row timed something else. Never fatal: an API that cannot
// run says why.
#pragma once
#include <d3d9.h>
#include <d3d10.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace precisecheck {

using Microsoft::WRL::ComPtr;

constexpr UINT kW = 64, kH = 4;

const char* const kCheck =
    "float soptErr(float a, float b) { return saturate(abs(a - b) * 1000.0); }\n"
    "float4 soptCheck(float2 pos)\n"
    "{\n"
    "  float x = (pos.x - 32.0) * 0.37 + pos.y * 0.011;\n"
    "  float2 t = pos * 3.7;\n"
    "  precise float2 d = t - (t + 12582912.0 - 12582912.0);\n"
    "  float2 e = abs(d - (t - round(t)));\n"
    "  return float4(soptErr(roundAdd(x), round(x)), max(soptErr(floorAdd(x), floor(x)), soptErr(fracAdd(x), frac(x))),\n"
    "                saturate(max(e.x, e.y) * 1000.0), 1.0);\n"
    "}\n";

const char* const kVs =
    "float4 main(uint v : SV_VertexID) : SV_Position\n"
    "{ float2 p = float2((v << 1) & 2, v & 2); return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n";

ComPtr<ID3DBlob> compile(const std::string& src, const char* profile) {
  ComPtr<ID3DBlob> code;
  D3DCompile(src.data(), src.size(), "precise_check", nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
             &code, nullptr);
  return code;
}

// Channels 0 / 1 / 2 hold the error of round / floor and frac / the float2 round, 3 is 255 where the shader ran.
std::string verdict(const uint8_t* data, UINT pitch, int r, int g, int b) {
  int err[3] = {0, 0, 0};
  int ran = 0;
  const int order[3] = {r, g, b};
  const int a = 6 - r - g - b;
  for (UINT y = 0; y < kH; ++y)
    for (UINT x = 0; x < kW; ++x) {
      const uint8_t* p = data + y * pitch + x * 4;
      for (int c = 0; c < 3; ++c)
        if (p[order[c]] > err[c]) err[c] = p[order[c]];
      if (p[a] == 255) ++ran;
    }
  if (ran != int(kW * kH)) return "not run (nothing drawn)";
  std::string wrong;
  const char* names[3] = {"round", "floor-frac", "float2-round"};
  for (int c = 0; c < 3; ++c)
    if (err[c] > 8) wrong += std::string(wrong.empty() ? "" : " ") + names[c];
  return wrong.empty() ? "right" : "WRONG (" + wrong + ")";
}

std::string runD3d9(const std::string& forms, UINT vendor, UINT device) {
  ComPtr<IDirect3D9> d3d;
  d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
  if (!d3d) return "not run (no Direct3D 9)";
  UINT ordinal = UINT(-1);
  for (UINT i = 0; i < d3d->GetAdapterCount() && ordinal == UINT(-1); ++i) {
    D3DADAPTER_IDENTIFIER9 id = {};
    if (SUCCEEDED(d3d->GetAdapterIdentifier(i, 0, &id)) && id.VendorId == vendor && id.DeviceId == device) ordinal = i;
  }
  if (ordinal == UINT(-1)) return "not run (no monitor on this card)";
  HWND hwnd = CreateWindowA("STATIC", "", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, nullptr, nullptr);
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  std::string result = "not run (no device)";
  {
    ComPtr<IDirect3DDevice9> dev;
    if (SUCCEEDED(d3d->CreateDevice(ordinal, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
      const std::string psSrc = forms + kCheck + "float4 main(float2 pos : VPOS) : COLOR { return soptCheck(pos + 0.5); }\n";
      const ComPtr<ID3DBlob> vsCode = compile("float4 main(float2 p : POSITION) : POSITION { return float4(p, 0.0, 1.0); }\n", "vs_3_0");
      const ComPtr<ID3DBlob> psCode = compile(psSrc, "ps_3_0");
      ComPtr<IDirect3DVertexShader9> vs;
      ComPtr<IDirect3DPixelShader9> ps;
      ComPtr<IDirect3DSurface9> rt, sys;
      ComPtr<IDirect3DVertexDeclaration9> decl;
      const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
      if (vsCode && psCode && SUCCEEDED(dev->CreateVertexShader(static_cast<const DWORD*>(vsCode->GetBufferPointer()), &vs)) &&
          SUCCEEDED(dev->CreatePixelShader(static_cast<const DWORD*>(psCode->GetBufferPointer()), &ps)) &&
          SUCCEEDED(dev->CreateVertexDeclaration(elems, &decl)) &&
          SUCCEEDED(dev->CreateRenderTarget(kW, kH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
          SUCCEEDED(dev->CreateOffscreenPlainSurface(kW, kH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr))) {
        const float tri[] = {-1.0f, 3.0f, 3.0f, -1.0f, -1.0f, -1.0f};
        dev->SetRenderTarget(0, rt.Get());
        dev->SetVertexShader(vs.Get());
        dev->SetPixelShader(ps.Get());
        dev->SetVertexDeclaration(decl.Get());
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);
        dev->BeginScene();
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 8);
        dev->EndScene();
        D3DLOCKED_RECT lr = {};
        if (SUCCEEDED(dev->GetRenderTargetData(rt.Get(), sys.Get())) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
          result = verdict(static_cast<const uint8_t*>(lr.pBits), UINT(lr.Pitch), 2, 1, 0);  // BGRA
          sys->UnlockRect();
        }
      }
    }
  }
  DestroyWindow(hwnd);
  return result;
}

std::string runD3d10(const std::string& forms, IDXGIAdapter* adapter) {
  ComPtr<ID3D10Device> dev;
  if (FAILED(D3D10CreateDevice(adapter, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &dev))) return "not run (no device)";
  const ComPtr<ID3DBlob> vsCode = compile(kVs, "vs_4_0");
  const ComPtr<ID3DBlob> psCode = compile(forms + kCheck + "float4 main(float4 pos : SV_Position) : SV_Target { return soptCheck(pos.xy); }\n", "ps_4_0");
  D3D10_TEXTURE2D_DESC td = {};
  td.Width = kW;
  td.Height = kH;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D10_BIND_RENDER_TARGET;
  ComPtr<ID3D10Texture2D> rt, staging;
  ComPtr<ID3D10RenderTargetView> rtv;
  ComPtr<ID3D10VertexShader> vs;
  ComPtr<ID3D10PixelShader> ps;
  if (!vsCode || !psCode || FAILED(dev->CreateTexture2D(&td, nullptr, &rt)) || FAILED(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv)) ||
      FAILED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &vs)) ||
      FAILED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), &ps)))
    return "not run (setup failed)";
  td.BindFlags = 0;
  td.Usage = D3D10_USAGE_STAGING;
  td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return "not run (setup failed)";
  const D3D10_VIEWPORT vp = {0, 0, kW, kH, 0.0f, 1.0f};
  dev->RSSetViewports(1, &vp);
  ID3D10RenderTargetView* rtvs[] = {rtv.Get()};
  dev->OMSetRenderTargets(1, rtvs, nullptr);
  dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dev->VSSetShader(vs.Get());
  dev->PSSetShader(ps.Get());
  dev->Draw(3, 0);
  dev->CopyResource(staging.Get(), rt.Get());
  D3D10_MAPPED_TEXTURE2D m = {};
  if (FAILED(staging->Map(0, D3D10_MAP_READ, 0, &m))) return "not run (no read back)";
  const std::string result = verdict(static_cast<const uint8_t*>(m.pData), m.RowPitch, 0, 1, 2);
  staging->Unmap(0);
  return result;
}

// The pixel shader, then the compute shader (what OpBench times).
void runD3d11(const std::string& forms, IDXGIAdapter* adapter, std::string& psResult, std::string& csResult) {
  psResult = csResult = "not run (no device)";
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return;
  psResult = csResult = "not run (setup failed)";
  const ComPtr<ID3DBlob> vsCode = compile(kVs, "vs_5_0");
  const ComPtr<ID3DBlob> psCode = compile(forms + kCheck + "float4 main(float4 pos : SV_Position) : SV_Target { return soptCheck(pos.xy); }\n", "ps_5_0");
  const ComPtr<ID3DBlob> csCode = compile(forms + kCheck +
                                              "RWTexture2D<unorm float4> O : register(u0);\n"
                                              "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { O[id.xy] = soptCheck(float2(id.xy) + 0.5); }\n",
                                          "cs_5_0");
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = kW;
  td.Height = kH;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
  ComPtr<ID3D11Texture2D> rt, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11UnorderedAccessView> uav;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11ComputeShader> cs;
  if (!vsCode || !psCode || !csCode || FAILED(dev->CreateTexture2D(&td, nullptr, &rt)) ||
      FAILED(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv)) || FAILED(dev->CreateUnorderedAccessView(rt.Get(), nullptr, &uav)) ||
      FAILED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) ||
      FAILED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)) ||
      FAILED(dev->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, &cs)))
    return;
  td.BindFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return;
  auto read = [&]() -> std::string {
    ctx->CopyResource(staging.Get(), rt.Get());
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return "not run (no read back)";
    const std::string r = verdict(static_cast<const uint8_t*>(m.pData), m.RowPitch, 0, 1, 2);
    ctx->Unmap(staging.Get(), 0);
    return r;
  };
  const D3D11_VIEWPORT vp = {0.0f, 0.0f, float(kW), float(kH), 0.0f, 1.0f};
  ctx->RSSetViewports(1, &vp);
  ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
  ctx->OMSetRenderTargets(1, rtvs, nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(), nullptr, 0);
  ctx->PSSetShader(ps.Get(), nullptr, 0);
  ctx->Draw(3, 0);
  psResult = read();
  ID3D11RenderTargetView* noRtv = nullptr;
  ctx->OMSetRenderTargets(1, &noRtv, nullptr);
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ctx->ClearRenderTargetView(rtv.Get(), zero);
  ID3D11UnorderedAccessView* uavs[] = {uav.Get()};
  ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
  ctx->CSSetShader(cs.Get(), nullptr, 0);
  ctx->Dispatch(kW / 8, kH / 8 + 1, 1);
  ID3D11UnorderedAccessView* noUav = nullptr;
  ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  csResult = read();
}

std::string runD3d12(const std::string& forms, IDXGIAdapter* adapter) {
  HMODULE lib = LoadLibraryA("d3d12.dll");
  if (!lib) return "not run (no Direct3D 12)";
  auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(lib, "D3D12CreateDevice"));
  using SerializeRoot = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
  auto serializeRoot = reinterpret_cast<SerializeRoot>(GetProcAddress(lib, "D3D12SerializeRootSignature"));
  ComPtr<ID3D12Device> dev;
  if (!createDevice || !serializeRoot || FAILED(createDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
    return "not run (no device)";
  const ComPtr<ID3DBlob> vsCode = compile(kVs, "vs_5_0");
  const ComPtr<ID3DBlob> psCode = compile(forms + kCheck + "float4 main(float4 pos : SV_Position) : SV_Target { return soptCheck(pos.xy); }\n", "ps_5_0");
  if (!vsCode || !psCode) return "not run (setup failed)";

  D3D12_ROOT_SIGNATURE_DESC rd = {};
  ComPtr<ID3DBlob> rootCode, rootError;
  ComPtr<ID3D12RootSignature> root;
  if (FAILED(serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rootCode, &rootError)) ||
      FAILED(dev->CreateRootSignature(0, rootCode->GetBufferPointer(), rootCode->GetBufferSize(), IID_PPV_ARGS(&root))))
    return "not run (setup failed)";

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = root.Get();
  pd.VS = {vsCode->GetBufferPointer(), vsCode->GetBufferSize()};
  pd.PS = {psCode->GetBufferPointer(), psCode->GetBufferSize()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return "not run (setup failed)";

  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = kW;
  td.Height = kH;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> rt, readback;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt))))
    return "not run (setup failed)";
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT64 total = 0;
  dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    return "not run (setup failed)";

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))) || FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
      FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
      FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list))) ||
      FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    return "not run (setup failed)";
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
  dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);

  list->SetGraphicsRootSignature(root.Get());
  const D3D12_VIEWPORT vp = {0.0f, 0.0f, float(kW), float(kH), 0.0f, 1.0f};
  const D3D12_RECT sr = {0, 0, LONG(kW), LONG(kH)};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &sr);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = rt.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = fp;
  src.pResource = rt.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (FAILED(list->Close())) return "not run (setup failed)";
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  queue->Signal(fence.Get(), 1);
  fence->SetEventOnCompletion(1, done);
  WaitForSingleObject(done, 10000);
  CloseHandle(done);
  if (fence->GetCompletedValue() < 1) return "not run (timed out)";
  void* mapped = nullptr;
  if (FAILED(readback->Map(0, nullptr, &mapped))) return "not run (no read back)";
  const std::string result = verdict(static_cast<const uint8_t*>(mapped), fp.Footprint.RowPitch, 0, 1, 2);
  readback->Unmap(0, nullptr);
  return result;
}

// One line for the report: forms defines roundAdd, floorAdd and fracAdd.
inline std::string run(IDXGIAdapter* adapter, UINT vendor, UINT device, const std::string& forms) {
  std::string ps11, cs11;
  runD3d11(forms, adapter, ps11, cs11);
  return "D3D9 " + runD3d9(forms, vendor, device) + "; D3D10 " + runD3d10(forms, adapter) + "; D3D11 pixel " + ps11 +
         "; D3D11 compute " + cs11 + "; D3D12 " + runD3d12(forms, adapter);
}

}  // namespace precisecheck
