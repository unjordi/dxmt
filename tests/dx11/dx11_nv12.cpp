// Self-checking test for multi-planar (NV12/P010) textures.
// Exit code 0: all checks passed. Build: x86_64-w64-mingw32-g++ -O1 dx11_nv12.cpp -ld3d11 -ldxgi -luuid
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static int failures = 0;

#define CHECK(cond, ...)                                                                                               \
  do {                                                                                                                 \
    if (cond) {                                                                                                        \
      printf("PASS ");                                                                                                 \
    } else {                                                                                                           \
      printf("FAIL ");                                                                                                 \
      failures++;                                                                                                      \
    }                                                                                                                  \
    printf(__VA_ARGS__);                                                                                               \
    printf("\n");                                                                                                      \
  } while (0)

static const UINT W = 70, H = 38;

typedef HRESULT(WINAPI *PFN_D3DCOMPILE)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
    ID3DBlob **
);

static ID3DBlob *
compile(PFN_D3DCOMPILE pfn, const char *src, const char *entry, const char *target) {
  ID3DBlob *code = nullptr, *errors = nullptr;
  HRESULT hr = pfn(src, strlen(src), entry, nullptr, nullptr, entry, target, 0, 0, &code, &errors);
  if (FAILED(hr)) {
    printf("compile %s failed: %s\n", entry, errors ? (const char *)errors->GetBufferPointer() : "?");
    return nullptr;
  }
  return code;
}

static ID3D11Texture2D *
create_texture(ID3D11Device *dev, DXGI_FORMAT format, UINT w, UINT h, D3D11_USAGE usage, UINT bind, UINT cpu, HRESULT *phr) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = w;
  desc.Height = h;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = usage;
  desc.BindFlags = bind;
  desc.CPUAccessFlags = cpu;
  ID3D11Texture2D *tex = nullptr;
  *phr = dev->CreateTexture2D(&desc, nullptr, &tex);
  return tex;
}

static ID3D11RenderTargetView *
create_rtv(ID3D11Device *dev, ID3D11Resource *res, DXGI_FORMAT format, HRESULT *phr) {
  D3D11_RENDER_TARGET_VIEW_DESC desc = {};
  desc.Format = format;
  desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  ID3D11RenderTargetView *rtv = nullptr;
  *phr = dev->CreateRenderTargetView(res, &desc, &rtv);
  return rtv;
}

static ID3D11ShaderResourceView *
create_srv(ID3D11Device *dev, ID3D11Resource *res, DXGI_FORMAT format, HRESULT *phr) {
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = format;
  desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView *srv = nullptr;
  *phr = dev->CreateShaderResourceView(res, &desc, &srv);
  return srv;
}

// Copies a planar DEFAULT texture into a new STAGING texture and maps it.
static bool
read_back(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, DXGI_FORMAT format, ID3D11Texture2D **staging,
          D3D11_MAPPED_SUBRESOURCE *mapped) {
  HRESULT hr;
  *staging = create_texture(dev, format, W, H, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, &hr);
  CHECK(SUCCEEDED(hr), "CreateTexture2D STAGING fmt=%d hr=0x%08lx", format, hr);
  if (!*staging)
    return false;
  ctx->CopyResource(*staging, src);
  hr = ctx->Map(*staging, 0, D3D11_MAP_READ, 0, mapped);
  CHECK(SUCCEEDED(hr), "Map STAGING fmt=%d hr=0x%08lx RowPitch=%u DepthPitch=%u", format, hr, mapped->RowPitch,
        mapped->DepthPitch);
  return SUCCEEDED(hr);
}

static void
test_clear(ID3D11Device *dev, ID3D11DeviceContext *ctx, DXGI_FORMAT format, DXGI_FORMAT y_view, DXGI_FORMAT uv_view,
           UINT texel) {
  HRESULT hr;
  ID3D11Texture2D *tex = create_texture(
      dev, format, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, &hr
  );
  CHECK(SUCCEEDED(hr), "CreateTexture2D fmt=%d %ux%u DEFAULT RT|SRV hr=0x%08lx", format, W, H, hr);
  if (!tex)
    return;
  ID3D11RenderTargetView *rtv_y = create_rtv(dev, tex, y_view, &hr);
  CHECK(SUCCEEDED(hr), "RTV luma fmt=%d hr=0x%08lx", y_view, hr);
  ID3D11RenderTargetView *rtv_uv = create_rtv(dev, tex, uv_view, &hr);
  CHECK(SUCCEEDED(hr), "RTV chroma fmt=%d hr=0x%08lx", uv_view, hr);
  ID3D11ShaderResourceView *srv;
  srv = create_srv(dev, tex, y_view, &hr);
  CHECK(SUCCEEDED(hr), "SRV luma fmt=%d hr=0x%08lx", y_view, hr);
  if (srv)
    srv->Release();
  srv = create_srv(dev, tex, uv_view, &hr);
  CHECK(SUCCEEDED(hr), "SRV chroma fmt=%d hr=0x%08lx", uv_view, hr);
  if (srv)
    srv->Release();
  if (!rtv_y || !rtv_uv)
    return;

  // unorm values chosen to be exactly representable
  const float max = texel == 1 ? 255.0f : 65535.0f;
  const uint32_t y_value = texel == 1 ? 0x40 : 0x4000;
  const uint32_t u_value = texel == 1 ? 0x80 : 0x8000;
  const uint32_t v_value = texel == 1 ? 0x20 : 0x2000;
  float clear_y[4] = {y_value / max, 0, 0, 0};
  float clear_uv[4] = {u_value / max, v_value / max, 0, 0};
  ctx->ClearRenderTargetView(rtv_y, clear_y);
  ctx->ClearRenderTargetView(rtv_uv, clear_uv);

  ID3D11Texture2D *staging;
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (!read_back(dev, ctx, tex, format, &staging, &mapped))
    return;
  auto read = [&](const uint8_t *p) -> uint32_t { return texel == 1 ? *p : *(const uint16_t *)p; };
  const uint8_t *base = (const uint8_t *)mapped.pData;
  unsigned bad_y = 0, bad_uv = 0;
  for (UINT y = 0; y < H; y++)
    for (UINT x = 0; x < W; x++)
      bad_y += read(base + y * mapped.RowPitch + x * texel) != y_value;
  const uint8_t *uv = base + (size_t)mapped.RowPitch * H;
  for (UINT y = 0; y < H / 2; y++)
    for (UINT x = 0; x < W / 2; x++) {
      bad_uv += read(uv + y * mapped.RowPitch + x * 2 * texel) != u_value;
      bad_uv += read(uv + y * mapped.RowPitch + x * 2 * texel + texel) != v_value;
    }
  CHECK(bad_y == 0, "fmt=%d clear luma: %u/%u texels wrong (first=0x%x)", format, bad_y, W * H, read(base));
  CHECK(bad_uv == 0, "fmt=%d clear chroma at pData+RowPitch*H: %u/%u values wrong (first=0x%x,0x%x)", format, bad_uv,
        W * H / 2, read(uv), read(uv + texel));
  CHECK(mapped.RowPitch >= W * texel, "fmt=%d RowPitch %u >= %u", format, mapped.RowPitch, W * texel);
  ctx->Unmap(staging, 0);
  staging->Release();
  rtv_y->Release();
  rtv_uv->Release();
  tex->Release();
}

static const char *shaders = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps_y(float4 pos : SV_Position) : SV_Target {
  uint2 p = uint2(pos.xy);
  return float((p.x * 3 + p.y) % 256) / 255.0;
}
float2 ps_uv(float4 pos : SV_Position) : SV_Target {
  uint2 p = uint2(pos.xy);
  return float2(float((p.y * 5) % 256) / 255.0, float((p.x + p.y * 7 + 9) % 256) / 255.0);
}
Texture2D<float> plane_y : register(t0);
Texture2D<float2> plane_uv : register(t1);
float4 ps_read(float4 pos : SV_Position) : SV_Target {
  int2 p = int2(pos.xy);
  return float4(plane_y.Load(int3(p, 0)), plane_uv.Load(int3(p / 2, 0)), 1);
}
)";

static uint8_t expect_y(UINT x, UINT y) { return (x * 3 + y) % 256; }
static uint8_t expect_u(UINT x, UINT y) { return (y * 5) % 256; }
static uint8_t expect_v(UINT x, UINT y) { return (x + y * 7 + 9) % 256; }

static void
test_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
  HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
  auto pfn = compiler ? (PFN_D3DCOMPILE)GetProcAddress(compiler, "D3DCompile") : nullptr;
  if (!pfn) {
    printf("SKIP draw tests: d3dcompiler_47.dll not available\n");
    return;
  }
  ID3DBlob *vs_code = compile(pfn, shaders, "vs", "vs_5_0");
  ID3DBlob *ps_y_code = compile(pfn, shaders, "ps_y", "ps_5_0");
  ID3DBlob *ps_uv_code = compile(pfn, shaders, "ps_uv", "ps_5_0");
  ID3DBlob *ps_read_code = compile(pfn, shaders, "ps_read", "ps_5_0");
  CHECK(vs_code && ps_y_code && ps_uv_code && ps_read_code, "compile shaders");
  if (!vs_code || !ps_y_code || !ps_uv_code || !ps_read_code)
    return;
  ID3D11VertexShader *vs;
  ID3D11PixelShader *ps_y, *ps_uv, *ps_read;
  dev->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs);
  dev->CreatePixelShader(ps_y_code->GetBufferPointer(), ps_y_code->GetBufferSize(), nullptr, &ps_y);
  dev->CreatePixelShader(ps_uv_code->GetBufferPointer(), ps_uv_code->GetBufferSize(), nullptr, &ps_uv);
  dev->CreatePixelShader(ps_read_code->GetBufferPointer(), ps_read_code->GetBufferSize(), nullptr, &ps_read);

  HRESULT hr;
  ID3D11Texture2D *tex = create_texture(
      dev, DXGI_FORMAT_NV12, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, &hr
  );
  if (!tex)
    return;
  ID3D11RenderTargetView *rtv_y = create_rtv(dev, tex, DXGI_FORMAT_R8_UNORM, &hr);
  ID3D11RenderTargetView *rtv_uv = create_rtv(dev, tex, DXGI_FORMAT_R8G8_UNORM, &hr);

  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs, nullptr, 0);

  // Y plane, full resolution
  D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, &rtv_y, nullptr);
  ctx->PSSetShader(ps_y, nullptr, 0);
  ctx->Draw(3, 0);
  // UV plane, half resolution
  vp.Width = W / 2;
  vp.Height = H / 2;
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, &rtv_uv, nullptr);
  ctx->PSSetShader(ps_uv, nullptr, 0);
  ctx->Draw(3, 0);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);

  ID3D11Texture2D *staging;
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (read_back(dev, ctx, tex, DXGI_FORMAT_NV12, &staging, &mapped)) {
    const uint8_t *base = (const uint8_t *)mapped.pData;
    const uint8_t *uv = base + (size_t)mapped.RowPitch * H;
    unsigned bad_y = 0, bad_uv = 0;
    for (UINT y = 0; y < H; y++)
      for (UINT x = 0; x < W; x++)
        bad_y += base[y * mapped.RowPitch + x] != expect_y(x, y);
    for (UINT y = 0; y < H / 2; y++)
      for (UINT x = 0; x < W / 2; x++) {
        bad_uv += uv[y * mapped.RowPitch + x * 2] != expect_u(x, y);
        bad_uv += uv[y * mapped.RowPitch + x * 2 + 1] != expect_v(x, y);
      }
    CHECK(bad_y == 0, "draw luma: %u/%u texels wrong", bad_y, W * H);
    CHECK(bad_uv == 0, "draw chroma: %u/%u values wrong", bad_uv, W * H / 2);
    ctx->Unmap(staging, 0);
    staging->Release();
  }

  // sample both planes through SRVs into an RGBA8 target
  ID3D11ShaderResourceView *srvs[2] = {
      create_srv(dev, tex, DXGI_FORMAT_R8_UNORM, &hr), create_srv(dev, tex, DXGI_FORMAT_R8G8_UNORM, &hr)
  };
  ID3D11Texture2D *rgba = create_texture(
      dev, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, &hr
  );
  ID3D11RenderTargetView *rtv_rgba = create_rtv(dev, rgba, DXGI_FORMAT_R8G8B8A8_UNORM, &hr);
  vp.Width = W;
  vp.Height = H;
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, &rtv_rgba, nullptr);
  ctx->PSSetShader(ps_read, nullptr, 0);
  ctx->PSSetShaderResources(0, 2, srvs);
  ctx->Draw(3, 0);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);

  ID3D11Texture2D *rgba_staging = create_texture(
      dev, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, &hr
  );
  ctx->CopyResource(rgba_staging, rgba);
  if (SUCCEEDED(ctx->Map(rgba_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
    unsigned bad = 0;
    for (UINT y = 0; y < H; y++)
      for (UINT x = 0; x < W; x++) {
        const uint8_t *p = (const uint8_t *)mapped.pData + y * mapped.RowPitch + x * 4;
        bad += p[0] != expect_y(x, y) || p[1] != expect_u(x / 2, y / 2) || p[2] != expect_v(x / 2, y / 2);
      }
    CHECK(bad == 0, "sample luma+chroma SRVs: %u/%u pixels wrong", bad, W * H);
    ctx->Unmap(rgba_staging, 0);
  } else {
    CHECK(false, "Map RGBA staging");
  }
}

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx
  );
  CHECK(SUCCEEDED(hr), "D3D11CreateDevice hr=0x%08lx", hr);
  if (!dev)
    return 2;

  const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET |
                        D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                        D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
  UINT support = 0;
  dev->CheckFormatSupport(DXGI_FORMAT_NV12, &support);
  CHECK((support & required) == required, "CheckFormatSupport(NV12)=0x%08x", support);
  support = 0;
  dev->CheckFormatSupport(DXGI_FORMAT_P010, &support);
  CHECK((support & required) == required, "CheckFormatSupport(P010)=0x%08x", support);

  ID3D11Texture2D *odd = create_texture(
      dev, DXGI_FORMAT_NV12, W + 1, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, &hr
  );
  CHECK(odd == nullptr && hr == E_INVALIDARG, "NV12 with odd width rejected hr=0x%08lx", hr);

  test_clear(dev, ctx, DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, 1);
  test_clear(dev, ctx, DXGI_FORMAT_P010, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM, 2);
  test_draw(dev, ctx);

  printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
