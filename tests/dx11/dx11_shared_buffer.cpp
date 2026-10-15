// Self-checking test for buffers shared between processes (MISC_SHARED, MISC_SHARED_NTHANDLE,
// MISC_SHARED_KEYEDMUTEX). The parent creates a 400-byte constant buffer, writes it, and starts a child
// process that opens it, verifies the contents, and writes new contents back for the parent to verify.
// Exit code 0: all checks passed. Build: x86_64-w64-mingw32-g++ -O1 dx11_shared_buffer.cpp -ld3d11 -ldxgi -luuid
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
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

static const UINT kSize = 400;
static const wchar_t *kSharedName = L"dxmt_test_shared_buffer";

enum OpenMethod { OpenLegacy, OpenNtHandle, OpenByName };

struct Case {
  const char *name;
  D3D11_USAGE usage;
  UINT misc;
  OpenMethod open;
};

static const Case cases[] = {
    {"default-legacy", D3D11_USAGE_DEFAULT, D3D11_RESOURCE_MISC_SHARED, OpenLegacy},
    {"dynamic-legacy", D3D11_USAGE_DYNAMIC, D3D11_RESOURCE_MISC_SHARED, OpenLegacy},
    {"default-nt", D3D11_USAGE_DEFAULT, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, OpenNtHandle},
    {"dynamic-nt", D3D11_USAGE_DYNAMIC, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, OpenNtHandle},
    {"default-nt-name", D3D11_USAGE_DEFAULT, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
     OpenByName},
    {"dynamic-nt-name", D3D11_USAGE_DYNAMIC, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
     OpenByName},
    {"default-keyedmutex", D3D11_USAGE_DEFAULT, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, OpenLegacy},
    {"dynamic-keyedmutex-nt", D3D11_USAGE_DYNAMIC,
     D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, OpenNtHandle},
};
static const unsigned kCaseCount = sizeof(cases) / sizeof(cases[0]);

static void
fill_pattern(uint8_t *data, unsigned seed) {
  for (unsigned i = 0; i < kSize; i++)
    data[i] = (uint8_t)(i * 13 + seed * 29 + 1);
}

static unsigned
count_mismatches(const uint8_t *data, unsigned seed) {
  uint8_t expected[kSize];
  fill_pattern(expected, seed);
  unsigned n = 0;
  for (unsigned i = 0; i < kSize; i++)
    n += data[i] != expected[i];
  return n;
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
  for (int i = 0; i < 10000 && ctx->GetData(query, &done, sizeof(done), 0) != S_OK; i++)
    Sleep(1);
  query->Release();
}

// Copies the buffer into a STAGING buffer and returns its contents.
static bool
read_via_copy(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Buffer *buffer, uint8_t *out) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kSize;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *staging = nullptr;
  if (FAILED(dev->CreateBuffer(&desc, nullptr, &staging)))
    return false;
  ctx->CopyResource(staging, buffer);
  D3D11_MAPPED_SUBRESOURCE mapped;
  bool ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
  if (ok) {
    memcpy(out, mapped.pData, kSize);
    ctx->Unmap(staging, 0);
  }
  staging->Release();
  return ok;
}

typedef HRESULT(WINAPI *PFN_D3DCOMPILE)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
    ID3DBlob **
);

static const char *kCopyConstantsCS = R"(
cbuffer Constants : register(b0) { uint4 v[25]; };
RWByteAddressBuffer o : register(u0);
[numthreads(25, 1, 1)]
void main(uint i : SV_GroupIndex) { o.Store4(i * 16, v[i]); }
)";

// Binds the buffer as a constant buffer of a compute shader that copies it to a UAV, and returns the result.
// Returns 0 on success, 1 on failure, -1 if no shader compiler is available.
static int
read_via_constant_buffer(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Buffer *buffer, uint8_t *out) {
  static PFN_D3DCOMPILE pfn = nullptr;
  if (!pfn) {
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    pfn = compiler ? (PFN_D3DCOMPILE)GetProcAddress(compiler, "D3DCompile") : nullptr;
    if (!pfn)
      return -1;
  }
  ID3DBlob *code = nullptr, *errors = nullptr;
  if (FAILED(pfn(kCopyConstantsCS, strlen(kCopyConstantsCS), "cs", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                 &errors))) {
    printf("%scompile failed: %s\n", prefix, errors ? (const char *)errors->GetBufferPointer() : "?");
    return 1;
  }
  ID3D11ComputeShader *cs = nullptr;
  HRESULT hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
  code->Release();
  if (FAILED(hr))
    return 1;

  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kSize;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ID3D11Buffer *target = nullptr;
  if (FAILED(dev->CreateBuffer(&desc, nullptr, &target))) {
    cs->Release();
    return 1;
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.NumElements = kSize / 4;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ID3D11UnorderedAccessView *uav = nullptr;
  if (FAILED(dev->CreateUnorderedAccessView(target, &uav_desc, &uav))) {
    target->Release();
    cs->Release();
    return 1;
  }

  ctx->CSSetShader(cs, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, &buffer);
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch(1, 1, 1);
  ID3D11Buffer *null_buffer = nullptr;
  ID3D11UnorderedAccessView *null_uav = nullptr;
  ctx->CSSetConstantBuffers(0, 1, &null_buffer);
  ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
  bool ok = read_via_copy(dev, ctx, target, out);

  uav->Release();
  target->Release();
  cs->Release();
  return ok ? 0 : 1;
}

// Writes pattern `seed`: Map(WRITE_DISCARD) for DYNAMIC; UpdateSubresource or CopyResource for DEFAULT.
static bool
write_pattern(
    ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Buffer *buffer, D3D11_USAGE usage, unsigned seed,
    bool use_copy
) {
  uint8_t data[kSize];
  fill_pattern(data, seed);
  if (usage == D3D11_USAGE_DYNAMIC) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    CHECK(SUCCEEDED(hr) && mapped.pData, "Map(WRITE_DISCARD) shared dynamic buffer hr=0x%08lx", hr);
    if (FAILED(hr) || !mapped.pData)
      return false;
    memcpy(mapped.pData, data, kSize);
    ctx->Unmap(buffer, 0);
    return true;
  }
  if (!use_copy) {
    ctx->UpdateSubresource(buffer, 0, nullptr, data, 0, 0);
    return true;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kSize;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
  ID3D11Buffer *source = nullptr;
  HRESULT hr = dev->CreateBuffer(&desc, &init, &source);
  if (FAILED(hr))
    return false;
  ctx->CopyResource(buffer, source);
  source->Release();
  return true;
}

static void
verify_contents(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Buffer *buffer, unsigned seed, const char *what) {
  uint8_t data[kSize] = {};
  bool ok = read_via_copy(dev, ctx, buffer, data);
  unsigned bad = ok ? count_mismatches(data, seed) : kSize;
  CHECK(ok && bad == 0, "%s: CopyResource read-back (%u/%u bytes differ)", what, bad, kSize);

  memset(data, 0, sizeof(data));
  int r = read_via_constant_buffer(dev, ctx, buffer, data);
  if (r < 0) {
    printf("%sSKIP %s: constant buffer read (d3dcompiler_47.dll not available)\n", prefix, what);
    return;
  }
  bad = r == 0 ? count_mismatches(data, seed) : kSize;
  CHECK(r == 0 && bad == 0, "%s: constant buffer read by shader (%u/%u bytes differ)", what, bad, kSize);
}

static bool
create_device(ID3D11Device **dev, ID3D11DeviceContext **ctx) {
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev, nullptr, ctx
  );
  CHECK(SUCCEEDED(hr), "D3D11CreateDevice hr=0x%08lx", hr);
  return SUCCEEDED(hr);
}

static int
run_child(unsigned index) {
  static char child_prefix[64];
  snprintf(child_prefix, sizeof(child_prefix), "  [child %s] ", cases[index].name);
  prefix = child_prefix;
  const Case &c = cases[index];

  unsigned long long handle_value = 0;
  if (scanf("%llx", &handle_value) != 1 && c.open != OpenByName) {
    CHECK(false, "read shared handle from parent");
    return 1;
  }
  HANDLE handle = (HANDLE)(uintptr_t)handle_value;

  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  if (!create_device(&dev, &ctx))
    return 1;
  ID3D11Device1 *dev1 = nullptr;
  dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);

  ID3D11Buffer *buffer = nullptr;
  HRESULT hr = E_FAIL;
  switch (c.open) {
  case OpenLegacy:
    hr = dev->OpenSharedResource(handle, __uuidof(ID3D11Buffer), (void **)&buffer);
    CHECK(SUCCEEDED(hr) && buffer, "OpenSharedResource hr=0x%08lx", hr);
    break;
  case OpenNtHandle:
    hr = dev1 ? dev1->OpenSharedResource1(handle, __uuidof(ID3D11Buffer), (void **)&buffer) : E_NOINTERFACE;
    CHECK(SUCCEEDED(hr) && buffer, "OpenSharedResource1 hr=0x%08lx", hr);
    CloseHandle(handle);
    break;
  case OpenByName:
    hr = dev1 ? dev1->OpenSharedResourceByName(
                    kSharedName, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, __uuidof(ID3D11Buffer),
                    (void **)&buffer
                )
              : E_NOINTERFACE;
    CHECK(SUCCEEDED(hr) && buffer, "OpenSharedResourceByName hr=0x%08lx", hr);
    if (handle)
      CloseHandle(handle);
    break;
  }
  if (!buffer)
    return failures ? failures : 1;

  D3D11_BUFFER_DESC desc;
  buffer->GetDesc(&desc);
  CHECK(
      desc.ByteWidth == kSize && desc.Usage == c.usage && desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER,
      "opened desc: ByteWidth=%u Usage=%d BindFlags=0x%x", desc.ByteWidth, desc.Usage, desc.BindFlags
  );

  IDXGIKeyedMutex *mutex = nullptr;
  if (c.misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
    hr = buffer->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&mutex);
    CHECK(SUCCEEDED(hr), "QueryInterface(IDXGIKeyedMutex) hr=0x%08lx", hr);
    if (mutex) {
      hr = mutex->AcquireSync(1, 5000);
      CHECK(hr == S_OK, "AcquireSync(1) hr=0x%08lx", hr);
    }
  }

  verify_contents(dev, ctx, buffer, index * 2, "parent -> child");
  write_pattern(dev, ctx, buffer, c.usage, index * 2 + 1, true);
  wait_gpu_idle(dev, ctx);

  if (mutex) {
    hr = mutex->ReleaseSync(0);
    CHECK(hr == S_OK, "ReleaseSync(0) hr=0x%08lx", hr);
    mutex->Release();
  }

  buffer->Release();
  if (dev1)
    dev1->Release();
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
run_case(ID3D11Device *dev, ID3D11DeviceContext *ctx, unsigned index) {
  const Case &c = cases[index];
  printf("== %s\n", c.name);

  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kSize;
  desc.Usage = c.usage;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.CPUAccessFlags = c.usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
  desc.MiscFlags = c.misc;
  ID3D11Buffer *buffer = nullptr;
  HRESULT hr = dev->CreateBuffer(&desc, nullptr, &buffer);
  CHECK(SUCCEEDED(hr) && buffer, "CreateBuffer hr=0x%08lx", hr);
  if (!buffer)
    return;

  HANDLE handle = nullptr;
  bool nt = c.misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  if (nt) {
    IDXGIResource1 *resource1 = nullptr;
    hr = buffer->QueryInterface(__uuidof(IDXGIResource1), (void **)&resource1);
    if (SUCCEEDED(hr)) {
      hr = resource1->CreateSharedHandle(
          nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
          c.open == OpenByName ? kSharedName : nullptr, &handle
      );
      resource1->Release();
    }
    CHECK(SUCCEEDED(hr) && handle, "IDXGIResource1::CreateSharedHandle hr=0x%08lx", hr);
  } else {
    IDXGIResource *resource = nullptr;
    hr = buffer->QueryInterface(__uuidof(IDXGIResource), (void **)&resource);
    if (SUCCEEDED(hr)) {
      hr = resource->GetSharedHandle(&handle);
      resource->Release();
    }
    CHECK(SUCCEEDED(hr) && handle, "IDXGIResource::GetSharedHandle hr=0x%08lx", hr);
  }
  if (!handle) {
    buffer->Release();
    return;
  }

  IDXGIKeyedMutex *mutex = nullptr;
  if (c.misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
    hr = buffer->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&mutex);
    CHECK(SUCCEEDED(hr), "QueryInterface(IDXGIKeyedMutex) hr=0x%08lx", hr);
    if (mutex) {
      hr = mutex->AcquireSync(0, 5000);
      CHECK(hr == S_OK, "AcquireSync(0) hr=0x%08lx", hr);
    }
  }

  write_pattern(dev, ctx, buffer, c.usage, index * 2, false);
  wait_gpu_idle(dev, ctx);
  if (mutex) {
    hr = mutex->ReleaseSync(1);
    CHECK(hr == S_OK, "ReleaseSync(1) hr=0x%08lx", hr);
  }

  DWORD code = spawn_child(index, handle, nt);
  CHECK(code == 0, "child process exit code %ld", (long)code);

  if (mutex) {
    hr = mutex->AcquireSync(0, 5000);
    CHECK(hr == S_OK, "AcquireSync(0) after child hr=0x%08lx", hr);
  }
  verify_contents(dev, ctx, buffer, index * 2 + 1, "child -> parent");
  if (mutex) {
    mutex->ReleaseSync(0);
    mutex->Release();
  }

  if (nt)
    CloseHandle(handle);
  buffer->Release();
}

static void
run_invalid_cases(ID3D11Device *dev) {
  printf("== invalid\n");
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kSize;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  ID3D11Buffer *buffer = nullptr;
  HRESULT hr = dev->CreateBuffer(&desc, nullptr, &buffer);
  CHECK(hr == E_INVALIDARG && !buffer, "CreateBuffer(STAGING, SHARED) rejected hr=0x%08lx", hr);
  if (buffer)
    buffer->Release();

  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.CPUAccessFlags = 0;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  buffer = nullptr;
  hr = dev->CreateBuffer(&desc, nullptr, &buffer);
  CHECK(hr == E_INVALIDARG && !buffer, "CreateBuffer(SHARED | SHARED_KEYEDMUTEX) rejected hr=0x%08lx", hr);
  if (buffer)
    buffer->Release();
}

int
main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 3 && !strcmp(argv[1], "child")) {
    unsigned index = (unsigned)atoi(argv[2]);
    if (index >= kCaseCount)
      return 1;
    return run_child(index);
  }

  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  if (!create_device(&dev, &ctx))
    return 2;

  for (unsigned i = 0; i < kCaseCount; i++)
    run_case(dev, ctx, i);
  run_invalid_cases(dev);

  ctx->Release();
  dev->Release();
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
