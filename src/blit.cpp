// Draws GTA's frame into a VR eye image with a tiny shader: works for any swapchain format SteamVR offers
// (GTA renders BGRA, SteamVR may only offer RGBA), crops the centre to the eye's aspect, fixes sRGB.
// GTA's whole D3D11 state is saved/restored with ID3D11DeviceContext1::SwapDeviceContextState.
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cstring>
#include "blit.h"
#include "log.h"

namespace {
ID3D11Device* g_dev = nullptr;
ID3DDeviceContextState* g_state = nullptr;
ID3D11VertexShader* g_vs = nullptr; ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_smp = nullptr; ID3D11Buffer* g_cb = nullptr;
ID3D11RasterizerState* g_rs = nullptr; ID3D11BlendState* g_bs = nullptr; ID3D11DepthStencilState* g_ds = nullptr;
ID3D11Texture2D* g_tmp = nullptr; ID3D11ShaderResourceView* g_srv = nullptr; D3D11_TEXTURE2D_DESC g_tmpDesc{};
bool g_ok = false;
float g_mm[8] = {0.f,0.806f,0.147f,1.f,-1,-1,-1,-1};   // minimap overlay: source rect (backbuffer uv), dest rect (eye uv), dest x<0 = off
// 0.5.0: destination anchor (x, y, height in eye uv) + on flag; the source rect comes from the game (real radar position)
float g_mmDst[3] = {0.20f, 0.66f, 0.22f}; bool g_mmOn = true;
// 0.5.3 comfort vignette: x = current darkness 0..1 (set by the game script while moving / turning), y = inner radius
volatile float g_vig[2] = {0.f, 0.55f};
void mmRecalc(float srcAspect) {   // srcAspect = radar width / height in pixels
    float w = g_mmDst[2] * srcAspect; if (w > 0.6f) w = 0.6f;
    float x1 = g_mmDst[0] + w, y1 = g_mmDst[1] + g_mmDst[2];
    g_mm[4] = g_mmOn ? g_mmDst[0] : -1.f; g_mm[5] = g_mmDst[1]; g_mm[6] = x1 > 1.f ? 1.f : x1; g_mm[7] = y1 > 1.f ? 1.f : y1;
}

const char* kHlsl =
"cbuffer C : register(b0) { float4 p; float4 m; float4 d; float4 v; };\n"
"Texture2D t : register(t0); SamplerState s : register(s0);\n"
"struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vs(uint id : SV_VertexID) { VO o; float2 uv = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
"float4 ps(VO i) : SV_Target { float2 uv = float2(p.y + i.uv.x * p.x, i.uv.y);\n"
"  bool mm = d.x >= 0 && i.uv.x >= d.x && i.uv.x <= d.z && i.uv.y >= d.y && i.uv.y <= d.w;\n"
"  if (mm) uv = lerp(m.xy, m.zw, (i.uv - d.xy) / (d.zw - d.xy));\n"
"  float4 c = t.Sample(s, uv); if (p.w > 0.5) c = float4(i.uv.x, i.uv.y, 0.5 + 0.5 * step(0.5, frac(i.uv.x * 8)), 1);\n"
"  if (!mm && v.x > 0.001) { float r = length((i.uv - 0.5) * 2.0); c.rgb *= 1.0 - v.x * smoothstep(v.y, v.y + 0.35, r); }\n"
"  if (p.z > 0.5) c.rgb = pow(abs(c.rgb), 2.2); return float4(c.rgb, 1); }\n";

template<class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
}

namespace blit {
bool init(ID3D11Device* dev) {
    if (g_ok && g_dev == dev) return true;
    g_dev = dev;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { vrlog::write("blit: d3dcompiler_47.dll not available"); return false; }
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "gta5vr", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(compile(kHlsl, strlen(kHlsl), "gta5vr", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, &err))) {
        vrlog::write("blit: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        rel(vb); rel(pb); rel(err); return false;
    }
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_ps);
    rel(vb); rel(pb); rel(err);
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_smp);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &g_bs);
    D3D11_DEPTH_STENCIL_DESC dd{}; dev->CreateDepthStencilState(&dd, &g_ds);
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1))) {
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        UINT flags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        d1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_state);
        d1->Release();
    }
    if (!g_state) vrlog::write("blit: no D3D11.1 context state - GTA state saved manually is not supported");
    g_ok = g_vs && g_ps && g_smp && g_cb && g_rs && g_bs && g_ds && g_state;
    vrlog::write("blit: %s", g_ok ? "ready" : "init failed");
    return g_ok;
}

bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb, ID3D11Texture2D* eyeTex, DXGI_FORMAT rtvFormat, bool linearize, float eyeAspect, bool testPattern, bool copySource) {
    if (!g_ok) return false;
    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
    // the backbuffer usually has no shader-resource binding: copy it to our own texture first
    if (!g_tmp || g_tmpDesc.Width != bd.Width || g_tmpDesc.Height != bd.Height || g_tmpDesc.Format != bd.Format) {
        rel(g_srv); rel(g_tmp);
        D3D11_TEXTURE2D_DESC t = bd; t.SampleDesc = {1, 0}; t.MipLevels = 1; t.ArraySize = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE; t.CPUAccessFlags = 0; t.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&t, nullptr, &g_tmp))) { vrlog::write("blit: temp texture failed (format %d)", (int)bd.Format); return false; }
        g_dev->CreateShaderResourceView(g_tmp, nullptr, &g_srv);
        g_tmpDesc = bd;
        copySource = true;
    }
    if (copySource) { if (bd.SampleDesc.Count > 1) ctx->ResolveSubresource(g_tmp, 0, bb, 0, bd.Format); else ctx->CopyResource(g_tmp, bb); }

    D3D11_TEXTURE2D_DESC ed; eyeTex->GetDesc(&ed);
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = rtvFormat; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_dev->CreateRenderTargetView(eyeTex, &rv, &rtv))) {
        if (FAILED(g_dev->CreateRenderTargetView(eyeTex, nullptr, &rtv))) { vrlog::write("blit: eye render target failed"); return false; }
    }
    // centre crop: the game camera's vertical fov = eye fov, so take a slice of width height*eyeAspect
    float sx = (float)bd.Height * eyeAspect / (float)bd.Width; if (sx > 1.f) sx = 1.f;
    float cb[16] = {sx, 0.5f - 0.5f * sx, linearize ? 1.f : 0.f, testPattern ? 1.f : 0.f};
    cb[12] = g_vig[0]; cb[13] = g_vig[1];
    // minimap: the radar sits in the bottom-left corner of the frame, which the centre crop cuts away; it is copied
    // into the visible part. 0.5.0: the source rect is the radar's real place (from the game), not a 16:9 guess.
    if (g_mm[4] >= 0.f) {
        for (int i = 0; i < 4; ++i) { cb[4 + i] = g_mm[i]; cb[8 + i] = g_mm[4 + i]; }
    } else { cb[8] = cb[9] = cb[10] = cb[11] = -1.f; }

    ID3D11DeviceContext1* c1 = nullptr; ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1);
    if (!c1) { rtv->Release(); return false; }
    ID3DDeviceContextState* gta = nullptr;
    c1->SwapDeviceContextState(g_state, &gta);          // save GTA's state, switch to ours
    c1->ClearState();
    c1->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp{0, 0, (float)ed.Width, (float)ed.Height, 0, 1};
    c1->RSSetViewports(1, &vp); c1->RSSetState(g_rs);
    c1->OMSetRenderTargets(1, &rtv, nullptr); c1->OMSetBlendState(g_bs, nullptr, 0xffffffff); c1->OMSetDepthStencilState(g_ds, 0);
    c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c1->IASetInputLayout(nullptr);
    c1->VSSetShader(g_vs, nullptr, 0); c1->PSSetShader(g_ps, nullptr, 0);
    c1->PSSetShaderResources(0, 1, &g_srv); c1->PSSetSamplers(0, 1, &g_smp); c1->PSSetConstantBuffers(0, 1, &g_cb);
    c1->Draw(3, 0);
    c1->ClearState();
    c1->SwapDeviceContextState(gta, nullptr);           // back to GTA exactly as it was
    if (gta) gta->Release();
    c1->Release(); rtv->Release();
    return true;
}

void reset() { rel(g_srv); rel(g_tmp); }
void setVignette(float amount, float inner) { g_vig[1] = inner; g_vig[0] = amount; }
void setMinimap(bool on, float dx, float dy, float size) {
    g_mmOn = on; g_mmDst[0] = dx; g_mmDst[1] = dy; g_mmDst[2] = size;
    float a = (g_mm[3] > g_mm[1]) ? (g_mm[2] - g_mm[0]) * (16.f / 9.f) / (g_mm[3] - g_mm[1]) : 1.2f;
    mmRecalc(a);
}
void setMinimapSource(float sx0, float sy0, float sx1, float sy1, float screenAspect) {
    g_mm[0] = sx0; g_mm[1] = sy0; g_mm[2] = sx1; g_mm[3] = sy1;
    mmRecalc((sx1 - sx0) * screenAspect / (sy1 - sy0));
}
}
