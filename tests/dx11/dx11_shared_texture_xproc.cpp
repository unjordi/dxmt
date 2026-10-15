// Self-checking test for textures shared between processes, in the order used by SteamVR direct mode
// (driver_vrlink): the creating process hands out the handle right after CreateTexture2D, the other process
// writes the texture first, and the creator reads it afterwards.
// Cases: an RGBA8 swap texture (MISC_SHARED) and an NV12 encoder texture (MISC_SHARED, and
// MISC_SHARED_KEYEDMUTEX | MISC_SHARED_NTHANDLE), each written by the child and read back by the parent,
// plus the reverse direction for NV12.
// Exit code 0: all checks passed. Build: x86_64-w64-mingw32-g++ -O1 dx11_shared_texture_xproc.cpp -ld3d11 -ldxgi -luuid
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static const char *prefix = "";

#define CHECK(cond, ...)                                                                                               \
  do {                                                                                                                 \
    if (cond) {                                                                                                        \
      printf("%sPASS ", prefix);                                                                                       \
    } else {                                                                                                           \
      printf("%sFAIL ", prefix);                                                                                       \
      failures++;                                                                                                      \
    }                                                                                                                  \
    printf(__VA_ARGS__);                                                                                               \
    printf("\n");                                                                                                      \
  } while (0)

static const UINT kWidth = 256;
static const UINT kHeight = 128;

enum Kind { KindRgba, KindNv12 };
enum Direction { ChildWrites, ParentWrites };

struct Case {
  const char *name;
  Kind kind;
  UINT misc;
  Direction direction;
};

static const Case cases[] = {
    {"rgba-legacy-child-writes", KindRgba, D3D11_RESOURCE_MISC_SHARED, ChildWrites},
    {"nv12-legacy-child-writes", KindNv12, D3D11_RESOURCE_MISC_SHARED, ChildWrites},
    {"nv12-legacy-parent-writes", KindNv12, D3D11_RESOURCE_MISC_SHARED, ParentWrites},
    {"nv12-keyedmutex-nt-child-writes", KindNv12,
     D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, ChildWrites},
};
static const unsigned kCaseCount = sizeof(cases) / sizeof(cases[0]);

// Values written by the writer: RGBA (r,g,b,a) or NV12 (Y, Cb, Cr).
static const float kRgba[4] = {128.0f / 255, 64.0f / 255, 191.0f / 255, 1.0f};
static const uint8_t kRgbaBytes[4] = {128, 64, 191, 255};
static const float kLuma[4] = {87.0f / 255, 0, 0, 0};
static const float kChroma[4] = {184.0f / 255, 154.0f / 255, 0, 0};

static bool
create_device(ID3D11Device **dev, ID3D11DeviceContext **ctx) {
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev, nullptr, ctx
  );
  CHECK(SUCCEEDED(hr), "D3D11CreateDevice hr=0x%08lx", hr);
  return SUCCEEDED(hr);
}

static void
wait_gpu_idle(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  ID3D11Query *query = nullptr;
  if (FAILED(dev->CreateQuery(&qd, &query)))
    return;
  ctx->End(query);
  ctx->Flush();
  BOOL done = FALSE;
  for (int i = 0; i < 5000 && ctx->GetData(query, &done, sizeof(done), 0) != S_OK; i++)
    Sleep(1);
  query->Release();
}

static D3D11_TEXTURE2D_DESC
texture_desc(const Case &c) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = c.kind == KindNv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_R8G8B8A8_TYPELESS;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = c.kind == KindNv12 ? D3D11_BIND_RENDER_TARGET
                                      : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = c.misc;
  return desc;
}

// Clears the texture with the expected values through render target views (plane views for NV12).
static void
write_texture(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const Case &c) {
  D3D11_RENDER_TARGET_VIEW_DESC rd = {};
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  ID3D11RenderTargetView *rtv = nullptr;
  HRESULT hr;
  if (c.kind == KindRgba) {
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = dev->CreateRenderTargetView(tex, &rd, &rtv);
    CHECK(SUCCEEDED(hr), "CreateRenderTargetView R8G8B8A8_UNORM hr=0x%08lx", hr);
    if (rtv) {
      ctx->ClearRenderTargetView(rtv, kRgba);
      rtv->Release();
    }
  } else {
    rd.Format = DXGI_FORMAT_R8_UNORM;
    hr = dev->CreateRenderTargetView(tex, &rd, &rtv);
    CHECK(SUCCEEDED(hr), "CreateRenderTargetView R8_UNORM (luma plane) hr=0x%08lx", hr);
    if (rtv) {
      ctx->ClearRenderTargetView(rtv, kLuma);
      rtv->Release();
      rtv = nullptr;
    }
    rd.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = dev->CreateRenderTargetView(tex, &rd, &rtv);
    CHECK(SUCCEEDED(hr), "CreateRenderTargetView R8G8_UNORM (chroma plane) hr=0x%08lx", hr);
    if (rtv) {
      ctx->ClearRenderTargetView(rtv, kChroma);
      rtv->Release();
    }
  }
  wait_gpu_idle(dev, ctx);
}

// Reads the texture back through a STAGING copy and compares the center texel with the written values.
static void
verify_texture(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const Case &c, const char *what) {
  D3D11_TEXTURE2D_DESC desc = texture_desc(c);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = nullptr;
  HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &staging);
  CHECK(SUCCEEDED(hr), "%s: CreateTexture2D(STAGING) hr=0x%08lx", what, hr);
  if (!staging)
    return;
  ctx->CopyResource(staging, tex);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  CHECK(SUCCEEDED(hr), "%s: Map(READ) hr=0x%08lx", what, hr);
  if (SUCCEEDED(hr)) {
    auto *base = (const uint8_t *)mapped.pData;
    if (c.kind == KindRgba) {
      const uint8_t *px = base + (kHeight / 2) * mapped.RowPitch + (kWidth / 2) * 4;
      CHECK(
          !memcmp(px, kRgbaBytes, 4), "%s: texel (%u,%u,%u,%u), expected (128,64,191,255)", what, px[0], px[1],
          px[2], px[3]
      );
    } else {
      const uint8_t *y = base + (kHeight / 2) * mapped.RowPitch + kWidth / 2;
      const uint8_t *uv = base + kHeight * mapped.RowPitch + (kHeight / 4) * mapped.RowPitch + kWidth / 2;
      CHECK(
          y[0] == 87 && uv[0] == 184 && uv[1] == 154, "%s: Y=%u Cb=%u Cr=%u, expected Y=87 Cb=184 Cr=154", what,
          y[0], uv[0], uv[1]
      );
    }
    ctx->Unmap(staging, 0);
  }
  staging->Release();
}

static IDXGIKeyedMutex *
keyed_mutex(ID3D11Texture2D *tex, const Case &c) {
  if (!(c.misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))
    return nullptr;
  IDXGIKeyedMutex *mutex = nullptr;
  HRESULT hr = tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&mutex);
  CHECK(SUCCEEDED(hr), "QueryInterface(IDXGIKeyedMutex) hr=0x%08lx", hr);
  return mutex;
}

static int
run_child(unsigned index) {
  static char child_prefix[64];
  snprintf(child_prefix, sizeof(child_prefix), "  [child %s] ", cases[index].name);
  prefix = child_prefix;
  const Case &c = cases[index];

  unsigned long long handle_value = 0;
  if (scanf("%llx", &handle_value) != 1) {
    CHECK(false, "read shared handle from parent");
    return 1;
  }
  HANDLE handle = (HANDLE)(uintptr_t)handle_value;

  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  if (!create_device(&dev, &ctx))
    return 1;

  ID3D11Texture2D *tex = nullptr;
  HRESULT hr;
  if (c.misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
    ID3D11Device1 *dev1 = nullptr;
    dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
    hr = dev1 ? dev1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&tex) : E_NOINTERFACE;
    CHECK(SUCCEEDED(hr) && tex, "OpenSharedResource1 hr=0x%08lx", hr);
    CloseHandle(handle);
    if (dev1)
      dev1->Release();
  } else {
    hr = dev->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&tex);
    CHECK(SUCCEEDED(hr) && tex, "OpenSharedResource hr=0x%08lx", hr);
  }
  if (!tex)
    return failures ? failures : 1;

  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  D3D11_TEXTURE2D_DESC expected = texture_desc(c);
  CHECK(
      desc.Width == expected.Width && desc.Height == expected.Height && desc.Format == expected.Format,
      "opened desc: %ux%u format=%d", desc.Width, desc.Height, desc.Format
  );

  IDXGIKeyedMutex *mutex = keyed_mutex(tex, c);
  if (mutex) {
    hr = mutex->AcquireSync(0, 5000);
    CHECK(hr == S_OK, "AcquireSync(0) hr=0x%08lx", hr);
  }
  if (c.direction == ChildWrites)
    write_texture(dev, ctx, tex, c);
  else
    verify_texture(dev, ctx, tex, c, "parent -> child");
  if (mutex) {
    hr = mutex->ReleaseSync(1);
    CHECK(hr == S_OK, "ReleaseSync(1) hr=0x%08lx", hr);
    mutex->Release();
  }

  tex->Release();
  ctx->Release();
  dev->Release();
  return failures;
}

// Starts `exe child <index>`, passes `handle` (duplicated into the child if it is an NT handle) on its stdin.
static DWORD
spawn_child(unsigned index, HANDLE handle, bool nt_handle) {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  char cmdline[MAX_PATH + 64];
  snprintf(cmdline, sizeof(cmdline), "\"%s\" child %u", exe, index);

  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE read_end, write_end;
  if (!CreatePipe(&read_end, &write_end, &sa, 0))
    return ~0u;
  SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = read_end;
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    return ~0u;
  }
  CloseHandle(read_end);

  HANDLE child_handle = handle;
  if (nt_handle && !DuplicateHandle(
                       GetCurrentProcess(), handle, pi.hProcess, &child_handle, 0, FALSE, DUPLICATE_SAME_ACCESS
                   )) {
    child_handle = nullptr;
  }
  char line[32];
  int len = snprintf(line, sizeof(line), "%llx\n", (unsigned long long)(uintptr_t)child_handle);
  DWORD written;
  WriteFile(write_end, line, len, &written, nullptr);
  CloseHandle(write_end);

  fflush(stdout);
  ResumeThread(pi.hThread);
  DWORD code = ~0u;
  if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0)
    GetExitCodeProcess(pi.hProcess, &code);
  else
    TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code;
}

static void
run_case(unsigned index) {
  const Case &c = cases[index];
  printf("== %s\n", c.name);

  // a fresh device per case: the creator must not have submitted any work before the other process writes
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  if (!create_device(&dev, &ctx))
    return;

  D3D11_TEXTURE2D_DESC desc = texture_desc(c);
  ID3D11Texture2D *tex = nullptr;
  HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &tex);
  CHECK(SUCCEEDED(hr) && tex, "CreateTexture2D format=%d misc=0x%x hr=0x%08lx", desc.Format, desc.MiscFlags, hr);
  if (!tex) {
    ctx->Release();
    dev->Release();
    return;
  }

  HANDLE handle = nullptr;
  bool nt = c.misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  if (nt) {
    IDXGIResource1 *res1 = nullptr;
    hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1);
    if (SUCCEEDED(hr)) {
      hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
      res1->Release();
    }
    CHECK(SUCCEEDED(hr) && handle, "IDXGIResource1::CreateSharedHandle hr=0x%08lx", hr);
  } else {
    IDXGIResource *res = nullptr;
    hr = tex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
    if (SUCCEEDED(hr)) {
      hr = res->GetSharedHandle(&handle);
      res->Release();
    }
    CHECK(SUCCEEDED(hr) && handle, "IDXGIResource::GetSharedHandle hr=0x%08lx", hr);
  }

  IDXGIKeyedMutex *mutex = keyed_mutex(tex, c);
  if (c.direction == ParentWrites) {
    if (mutex) {
      hr = mutex->AcquireSync(0, 5000);
      CHECK(hr == S_OK, "AcquireSync(0) hr=0x%08lx", hr);
    }
    write_texture(dev, ctx, tex, c);
    if (mutex) {
      hr = mutex->ReleaseSync(0);
      CHECK(hr == S_OK, "ReleaseSync(0) hr=0x%08lx", hr);
    }
  }

  DWORD code = handle ? spawn_child(index, handle, nt) : ~0u;
  CHECK(code == 0, "child process exit code %ld", (long)code);
  if (nt && handle)
    CloseHandle(handle);

  if (c.direction == ChildWrites) {
    if (mutex) {
      hr = mutex->AcquireSync(1, 5000);
      CHECK(hr == S_OK, "AcquireSync(1) after child hr=0x%08lx", hr);
    }
    verify_texture(dev, ctx, tex, c, "child -> parent");
    if (mutex)
      mutex->ReleaseSync(0);
  }
  if (mutex)
    mutex->Release();
  tex->Release();
  ctx->Release();
  dev->Release();
}

int
main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 3 && !strcmp(argv[1], "child")) {
    unsigned index = (unsigned)atoi(argv[2]);
    return index < kCaseCount ? run_child(index) : 1;
  }
  for (unsigned i = 0; i < kCaseCount; i++)
    run_case(i);
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
