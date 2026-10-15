// Self-checking test for a keyed mutex shared between two processes, in the pattern of SteamVR direct mode.
// Parent (compositor role): creates a MISC_SHARED_KEYEDMUTEX sync texture and a MISC_SHARED texture; per frame
//   AcquireSync(0, 10), clear the texture to the frame index, Flush, signal the child, hold the mutex 5 ms,
//   ReleaseSync(0), wait up to 50 ms for the child to acquire.
// Child (driver role): opens both by legacy handle; per frame waits for the signal, AcquireSync(0, 100) while the
//   parent still holds the mutex, reads the texture through a STAGING copy, Flush, ReleaseSync(0).
// Checks: every child AcquireSync returns S_OK, no AcquireSync returns WAIT_TIMEOUT before its timeout elapsed,
// and the child reads the frame index written by the parent.
// Exit code 0: all checks passed. Usage: dx11_keyed_mutex_xproc.exe [frames]
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>

#define W 256
#define H 256

static ID3D11Device *dev; static ID3D11DeviceContext *ctx;
static LARGE_INTEGER freq;
static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / freq.QuadPart; }

static HRESULT mkdev(void) {
    return D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
}

static int child(HANDLE hsync, HANDLE hswap, int frames) {
    if (FAILED(mkdev())) return 2;
    HANDLE present = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, "dxmt_test_km_present");
    HANDLE acquired = OpenEventA(EVENT_MODIFY_STATE, FALSE, "dxmt_test_km_acquired");
    ID3D11Texture2D *sync = NULL, *swap = NULL, *st = NULL;
    HRESULT hr = ID3D11Device_OpenSharedResource(dev, hsync, &IID_ID3D11Texture2D, (void **)&sync);
    printf("[child] OpenSharedResource(sync) hr=0x%08lx\n", hr);
    hr = ID3D11Device_OpenSharedResource(dev, hswap, &IID_ID3D11Texture2D, (void **)&swap);
    printf("[child] OpenSharedResource(swap) hr=0x%08lx\n", hr);
    if (!sync || !swap) return 3;
    D3D11_TEXTURE2D_DESC sd = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st);
    int ok = 0, timeouts = 0, other = 0, relfail = 0, bad = 0, nopresent = 0; double worst = 0;
    for (int i = 0; i < frames; i++) {
        if (WaitForSingleObject(present, 2000) != WAIT_OBJECT_0) { nopresent++; if (nopresent > 3) break; continue; }
        IDXGIKeyedMutex *km = NULL;
        ID3D11Texture2D_QueryInterface(sync, &IID_IDXGIKeyedMutex, (void **)&km);
        double t0 = now_ms();
        hr = IDXGIKeyedMutex_AcquireSync(km, 0, 100);
        double dt = now_ms() - t0; if (dt > worst) worst = dt;
        if (hr == S_OK) {
            ok++;
            SetEvent(acquired);
            double t1 = now_ms();
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)swap);
            D3D11_MAPPED_SUBRESOURCE m = {0};
            HRESULT mh = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m);
            double mt = now_ms() - t1;
            if (SUCCEEDED(mh)) {
                unsigned char *p = (unsigned char *)m.pData + (H / 2) * m.RowPitch + (W / 2) * 4;
                if (p[0] != (unsigned char)(i & 0xff)) { if (bad < 5) printf("[child] frame %d color %u expected %u\n", i, p[0], i & 0xff); bad++; }
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
            }
            if (mt > 200) printf("[child] frame %d Map took %.1f ms\n", i, mt);
            ID3D11DeviceContext_Flush(ctx);
            HRESULT rh = IDXGIKeyedMutex_ReleaseSync(km, 0);
            if (rh != S_OK) { relfail++; if (relfail < 5) printf("[child] frame %d ReleaseSync hr=0x%08lx\n", i, rh); }
        } else {
            if (hr == WAIT_TIMEOUT) timeouts++; else other++;
            if (timeouts + other <= 5) printf("[child] frame %d AcquireSync(0,100) hr=0x%08lx after %.2f ms\n", i, hr, dt);
            IDXGIKeyedMutex_ReleaseSync(km, 0);
        }
        IDXGIKeyedMutex_Release(km);
    }
    printf("[child] summary acquire_ok=%d timeout=%d other=%d release_fail=%d bad_color=%d no_signal=%d worst_acquire=%.1fms\n",
           ok, timeouts, other, relfail, bad, nopresent, worst);
    return (timeouts || other || relfail || bad || nopresent) ? 1 : 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&freq);
    if (argc == 5 && !strcmp(argv[1], "child"))
        return child((HANDLE)(ULONG_PTR)strtoull(argv[2], NULL, 16), (HANDLE)(ULONG_PTR)strtoull(argv[3], NULL, 16), atoi(argv[4]));
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    if (FAILED(mkdev())) return 2;
    D3D11_TEXTURE2D_DESC d = {16, 16, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX};
    ID3D11Texture2D *sync = NULL, *swap = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &sync);
    printf("[parent] CreateTexture2D sync KEYEDMUTEX hr=0x%08lx\n", hr);
    D3D11_TEXTURE2D_DESC s = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
    hr = ID3D11Device_CreateTexture2D(dev, &s, NULL, &swap);
    printf("[parent] CreateTexture2D swap SHARED hr=0x%08lx\n", hr);
    if (!sync || !swap) return 2;
    HANDLE hsync = NULL, hswap = NULL; IDXGIResource *r = NULL;
    ID3D11Texture2D_QueryInterface(sync, &IID_IDXGIResource, (void **)&r); IDXGIResource_GetSharedHandle(r, &hsync); IDXGIResource_Release(r);
    ID3D11Texture2D_QueryInterface(swap, &IID_IDXGIResource, (void **)&r); IDXGIResource_GetSharedHandle(r, &hswap); IDXGIResource_Release(r);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)swap, NULL, &rtv);
    HANDLE present = CreateEventA(NULL, FALSE, FALSE, "dxmt_test_km_present");
    HANDLE acquired = CreateEventA(NULL, FALSE, FALSE, "dxmt_test_km_acquired");
    char cmd[512], self[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cmd, sizeof cmd, "\"%s\" child %p %p %d", self, hsync, hswap, frames);
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 3;
    Sleep(1500);
    int ok = 0, timeouts = 0, other = 0, relfail = 0, waitfail = 0, early = 0; double worst = 0;
    for (int i = 0; i < frames; i++) {
        IDXGIKeyedMutex *km = NULL;
        ID3D11Texture2D_QueryInterface(sync, &IID_IDXGIKeyedMutex, (void **)&km);
        /* retry until owned: the frame is only written while the parent holds the mutex */
        for (int attempt = 0; attempt < 50; attempt++) {
            double t0 = now_ms();
            hr = IDXGIKeyedMutex_AcquireSync(km, 0, 10);
            double dt = now_ms() - t0; if (dt > worst) worst = dt;
            if (hr == S_OK) { ok++; break; }
            if (hr == WAIT_TIMEOUT) timeouts++; else other++;
            if (timeouts + other <= 5) printf("[parent] frame %d AcquireSync(0,10) hr=0x%08lx after %.1f ms\n", i, hr, dt);
            if (hr == WAIT_TIMEOUT && dt < 9.0) { early++; if (early <= 5) printf("[parent] frame %d WAIT_TIMEOUT after %.2f ms (requested timeout 10 ms)\n", i, dt); }
            if (hr != WAIT_TIMEOUT) break;
        }
        if (hr != S_OK) { IDXGIKeyedMutex_Release(km); break; }
        float c[4] = {(i & 0xff) / 255.0f, 0, 0, 1};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        ID3D11DeviceContext_Flush(ctx);
        /* el driver intenta adquirir mienafter el compositor todavia la tiene (contencion de 5 ms) */
        SetEvent(present);
        Sleep(5);
        HRESULT rh = IDXGIKeyedMutex_ReleaseSync(km, 0);
        if (rh != S_OK) { relfail++; if (relfail < 5) printf("[parent] frame %d ReleaseSync hr=0x%08lx\n", i, rh); }
        IDXGIKeyedMutex_Release(km);
        if (WaitForSingleObject(acquired, 50) != WAIT_OBJECT_0) waitfail++;
        Sleep(6);
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { printf("[parent] child exited at frame %d\n", i); break; }
    }
    printf("[parent] summary acquire_ok=%d timeout=%d (early=%d) other=%d release_fail=%d child_acquire_missed=%d worst_acquire=%.1fms\n",
           ok, timeouts, early, other, relfail, waitfail, worst);
    SetEvent(present);
    DWORD ec = 99;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &ec);
    else { printf("[parent] child hung, TerminateProcess\n"); TerminateProcess(pi.hProcess, 9); }
    int pass = ec == 0 && !early && !other && !relfail && !waitfail;
    printf("TOTAL %s (child exit=%lu)\n", pass ? "PASS" : "FAIL", ec);
    return pass ? 0 : 1;
}
