// NVOF Motion - phase 1b: CUDA context + NvOF session + caps query.
//
// What this phase validates (still no frame processing):
//  - CUDA driver API present (nvcuda.dll, explicit GetProcAddress, no .lib)
//  - CUDA device matching the game's D3D12 adapter (LUID compare)
//  - own CUcontext create/destroy with SetCurrent discipline
//  - NvOFAPICreateInstanceCuda -> nvCreateOpticalFlowCuda -> nvOFInit
//  - nvOFGetCaps (W/H min/max, supported output grid sizes) shown in overlay
//
// Phase 2 will add: frame capture, NvOFExecute on worker thread, R16G16F output.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <imgui.h>
#include <reshade.hpp>

#include <cuda.h> // CUDA toolkit headers only (types); no cuda.lib linkage

#include "nvOpticalFlowCommon.h" // NVIDIAOpticalFlowSDK checkout (BSD)
#include "nvOpticalFlowCuda.h"

using namespace reshade;
using namespace reshade::api;

// ---- explicit CUDA driver-API imports (no import lib needed) ----
using cuInit_fn = CUresult(CUDAAPI *)(unsigned int);
using cuDeviceGetCount_fn = CUresult(CUDAAPI *)(int *);
using cuDeviceGet_fn = CUresult(CUDAAPI *)(CUdevice *, int);
using cuDeviceGetLuid_fn = CUresult(CUDAAPI *)(char *, unsigned int *, CUdevice);
using cuCtxCreate_fn = CUresult(CUDAAPI *)(CUcontext *, unsigned int, CUdevice);
using cuCtxDestroy_fn = CUresult(CUDAAPI *)(CUcontext);
using cuCtxSetCurrent_fn = CUresult(CUDAAPI *)(CUcontext);
using cuGetErrorString_fn = CUresult(CUDAAPI *)(CUresult, const char **);

using NvOFCreateInstance_fn =
    NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST *);

namespace {

std::mutex g_mutex;

HMODULE g_cuda_dll = nullptr;
HMODULE g_nvof_dll = nullptr;

cuInit_fn p_cuInit = nullptr;
cuDeviceGetCount_fn p_cuDeviceGetCount = nullptr;
cuDeviceGet_fn p_cuDeviceGet = nullptr;
cuDeviceGetLuid_fn p_cuDeviceGetLuid = nullptr;
cuCtxCreate_fn p_cuCtxCreate = nullptr;
cuCtxDestroy_fn p_cuCtxDestroy = nullptr;
cuCtxSetCurrent_fn p_cuCtxSetCurrent = nullptr;
cuGetErrorString_fn p_cuGetErrorString = nullptr;

NvOFCreateInstance_fn p_NvOFCreateInstanceCuda = nullptr;
NV_OF_CUDA_API_FUNCTION_LIST g_of = {};
NvOFHandle g_of_handle = nullptr;

CUcontext g_cu_ctx = nullptr;
bool g_session_active = false;
ID3D12Device *g_owner_device = nullptr;

unsigned int g_enabled = 1;
bool g_d3d12_seen = false;
std::string g_status = "not initialized yet";

// cached caps for the overlay (no CUDA calls from overlay thread)
uint32_t g_cap_wmin = 0, g_cap_wmax = 0, g_cap_hmin = 0, g_cap_hmax = 0;
uint32_t g_cap_grids = 0;
bool g_caps_ok = false;
int g_last_code = 0;

void set_status_locked(const char *text, reshade::log::level level)
{
    g_status.assign(text);
    reshade::log::message(level, text);
}

std::string cuda_error_text(CUresult res)
{
    const char *str = nullptr;
    if (p_cuGetErrorString != nullptr && p_cuGetErrorString(res, &str) == CUDA_SUCCESS && str != nullptr)
        return str;
    return "CUDA error " + std::to_string(static_cast<int>(res));
}

// Tries primary symbol name, then fallback (driver exports vary by version).
FARPROC load_sym(HMODULE mod, const char *primary, const char *fallback = nullptr)
{
    if (FARPROC p = GetProcAddress(mod, primary))
        return p;
    if (fallback != nullptr)
        return GetProcAddress(mod, fallback);
    return nullptr;
}

void teardown_session_locked()
{
    if (g_of_handle != nullptr && g_of.nvOFDestroy != nullptr)
    {
        if (g_cu_ctx != nullptr && p_cuCtxSetCurrent != nullptr)
            p_cuCtxSetCurrent(g_cu_ctx);
        g_of.nvOFDestroy(g_of_handle);
        g_of_handle = nullptr;
    }
    if (g_cu_ctx != nullptr)
    {
        if (p_cuCtxSetCurrent != nullptr)
            p_cuCtxSetCurrent(nullptr);
        if (p_cuCtxDestroy != nullptr)
            p_cuCtxDestroy(g_cu_ctx);
        g_cu_ctx = nullptr;
    }
    g_session_active = false;
    g_owner_device = nullptr;
    g_caps_ok = false;
}

} // namespace

static void on_init_device(device *device)
{
    if (g_enabled == 0)
        return;
    if (device->get_api() != device_api::d3d12)
        return; // v1 supports D3D12 only; stay silent elsewhere

    std::lock_guard<std::mutex> lock(g_mutex);
    g_d3d12_seen = true;

    if (g_session_active)
    {
        if (device->get_native() == reinterpret_cast<uint64_t>(g_owner_device))
            return; // already serving this device
        // Different device (multi-GPU): drop the old session and start clean.
    }
    teardown_session_locked();

    ID3D12Device *d3d12 = reinterpret_cast<ID3D12Device *>(device->get_native());
    if (d3d12 == nullptr)
    {
        set_status_locked("D3D12 device has no native handle", reshade::log::level::warning);
        return;
    }

    // ---- 1. CUDA driver API ----
    if (g_cuda_dll == nullptr)
    {
        g_cuda_dll = LoadLibraryW(L"nvcuda.dll");
        if (g_cuda_dll == nullptr)
        {
            set_status_locked("nvcuda.dll NOT found - no CUDA driver, staying idle", reshade::log::level::warning);
            return;
        }
        p_cuInit = reinterpret_cast<cuInit_fn>(load_sym(g_cuda_dll, "cuInit"));
        p_cuDeviceGetCount = reinterpret_cast<cuDeviceGetCount_fn>(load_sym(g_cuda_dll, "cuDeviceGetCount"));
        p_cuDeviceGet = reinterpret_cast<cuDeviceGet_fn>(load_sym(g_cuda_dll, "cuDeviceGet"));
        p_cuDeviceGetLuid = reinterpret_cast<cuDeviceGetLuid_fn>(load_sym(g_cuda_dll, "cuDeviceGetLuid"));
        p_cuCtxCreate = reinterpret_cast<cuCtxCreate_fn>(load_sym(g_cuda_dll, "cuCtxCreate", "cuCtxCreate_v2"));
        p_cuCtxDestroy = reinterpret_cast<cuCtxDestroy_fn>(load_sym(g_cuda_dll, "cuCtxDestroy", "cuCtxDestroy_v2"));
        p_cuCtxSetCurrent = reinterpret_cast<cuCtxSetCurrent_fn>(load_sym(g_cuda_dll, "cuCtxSetCurrent"));
        p_cuGetErrorString = reinterpret_cast<cuGetErrorString_fn>(load_sym(g_cuda_dll, "cuGetErrorString"));
        if (p_cuInit == nullptr || p_cuDeviceGetCount == nullptr || p_cuDeviceGet == nullptr ||
            p_cuDeviceGetLuid == nullptr || p_cuCtxCreate == nullptr || p_cuCtxDestroy == nullptr ||
            p_cuCtxSetCurrent == nullptr)
        {
            set_status_locked("nvcuda.dll present but driver entry points missing", reshade::log::level::warning);
            return;
        }
    }

    CUresult cu = p_cuInit(0);
    if (cu != CUDA_SUCCESS)
    {
        set_status_locked(cuda_error_text(cu).c_str(), reshade::log::level::warning);
        return;
    }

    // ---- 2. match CUDA device to the game's D3D12 adapter (LUID) ----
    const LUID adapter_luid = d3d12->GetAdapterLuid();
    int device_count = 0;
    cu = p_cuDeviceGetCount(&device_count);
    if (cu != CUDA_SUCCESS || device_count <= 0)
    {
        set_status_locked("no CUDA devices found", reshade::log::level::warning);
        return;
    }
    int matched = -1;
    for (int i = 0; i < device_count; ++i)
    {
        CUdevice dev = 0;
        char luid_bytes[8] = {};
        unsigned int node_mask = 0;
        if (p_cuDeviceGet(&dev, i) != CUDA_SUCCESS)
            continue;
        if (p_cuDeviceGetLuid(luid_bytes, &node_mask, dev) != CUDA_SUCCESS)
            continue;
        if (std::memcmp(luid_bytes, &adapter_luid, 8) == 0)
        {
            matched = static_cast<int>(dev);
            break;
        }
    }
    if (matched < 0)
    {
        set_status_locked("no CUDA device matches the game adapter (LUID mismatch)", reshade::log::level::warning);
        return;
    }

    // ---- 3. own CUDA context ----
    CUcontext ctx = nullptr;
    cu = p_cuCtxCreate(&ctx, 0 /* CU_CTX_SCHED_AUTO */, static_cast<CUdevice>(matched));
    if (cu != CUDA_SUCCESS || ctx == nullptr)
    {
        set_status_locked(cuda_error_text(cu).c_str(), reshade::log::level::warning);
        return;
    }
    // cuCtxCreate makes the context current on this thread; keep it current
    // for the NvOF calls below, detach at the end.
    g_cu_ctx = ctx;

    // ---- 4. NvOF function table ----
    if (g_nvof_dll == nullptr)
    {
        g_nvof_dll = LoadLibraryW(L"nvofapi64.dll");
        if (g_nvof_dll != nullptr)
            p_NvOFCreateInstanceCuda = reinterpret_cast<NvOFCreateInstance_fn>(
                GetProcAddress(g_nvof_dll, "NvOFAPICreateInstanceCuda"));
    }
    if (p_NvOFCreateInstanceCuda == nullptr)
    {
        set_status_locked("nvofapi64.dll / entry missing - staying idle", reshade::log::level::warning);
        goto detach_ctx;
    }
    g_of = {};
    {
        const NV_OF_STATUS st = p_NvOFCreateInstanceCuda(NV_OF_API_VERSION, &g_of);
        if (st != NV_OF_SUCCESS || g_of.nvCreateOpticalFlowCuda == nullptr || g_of.nvOFInit == nullptr ||
            g_of.nvOFGetCaps == nullptr || g_of.nvOFDestroy == nullptr)
        {
            g_last_code = static_cast<int>(st);
            set_status_locked("NvOFAPICreateInstanceCuda failed or incomplete", reshade::log::level::warning);
            goto detach_ctx;
        }
    }

    // ---- 5. session (placeholder 1280x720; phase 2 re-inits at real size) ----
    {
        NvOFHandle hOf = nullptr;
        NV_OF_STATUS st = g_of.nvCreateOpticalFlowCuda(ctx, &hOf);
        if (st != NV_OF_SUCCESS || hOf == nullptr)
        {
            g_last_code = static_cast<int>(st);
            set_status_locked("nvCreateOpticalFlowCuda failed", reshade::log::level::warning);
            goto detach_ctx;
        }
        g_of_handle = hOf;

        NV_OF_INIT_PARAMS init = {};
        init.width = 1280;
        init.height = 720;
        init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_4;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = NV_OF_PERF_LEVEL_FAST;
        init.enableExternalHints = NV_OF_FALSE;
        init.enableOutputCost = NV_OF_TRUE;
        init.hPrivData = nullptr;

        st = g_of.nvOFInit(hOf, &init);
        if (st != NV_OF_SUCCESS)
        {
            g_last_code = static_cast<int>(st);
            set_status_locked("nvOFInit failed", reshade::log::level::warning);
            teardown_session_locked(); // destroys handle + ctx, detaches current
            return;
        }

        // ---- 6. caps (best effort; failures are non-fatal) ----
        auto query_cap = [&](NV_OF_CAPS cap, uint32_t &out) {
            uint32_t val = 0;
            uint32_t size = sizeof(val);
            if (g_of.nvOFGetCaps(hOf, cap, &val, &size) == NV_OF_SUCCESS)
            {
                out = val;
                return true;
            }
            return false;
        };
        g_caps_ok = query_cap(NV_OF_CAPS_WIDTH_MIN, g_cap_wmin) &
            query_cap(NV_OF_CAPS_WIDTH_MAX, g_cap_wmax) &
            query_cap(NV_OF_CAPS_HEIGHT_MIN, g_cap_hmin) &
            query_cap(NV_OF_CAPS_HEIGHT_MAX, g_cap_hmax);
        query_cap(NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, g_cap_grids);

        g_session_active = true;
        g_owner_device = d3d12;
        set_status_locked("NvOF session active (1280x720, grid 4, fast) - phase 1b OK, no processing yet",
                          reshade::log::level::info);
    }

detach_ctx:
    if (p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(nullptr);
    return;
}

static void on_destroy_device(device *device)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_owner_device != nullptr &&
        device->get_native() == reinterpret_cast<uint64_t>(g_owner_device))
    {
        teardown_session_locked();
        g_status.assign("session torn down (device destroyed)");
    }
}

static void draw_overlay(effect_runtime *)
{
    std::string status;
    bool d3d12_seen = false;
    bool session = false;
    bool caps_ok = false;
    uint32_t wmin = 0, wmax = 0, hmin = 0, hmax = 0, grids = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        status = g_status;
        d3d12_seen = g_d3d12_seen;
        session = g_session_active;
        caps_ok = g_caps_ok;
        wmin = g_cap_wmin;
        wmax = g_cap_wmax;
        hmin = g_cap_hmin;
        hmax = g_cap_hmax;
        grids = g_cap_grids;
    }

    ImGui::TextUnformatted("NVOF Motion (phase 1b: session, no processing)");

    bool enabled = g_enabled != 0;
    if (ImGui::Checkbox("Enable", &enabled))
    {
        g_enabled = enabled ? 1u : 0u;
        reshade::set_config_value(nullptr, "NVOF", "Enable", g_enabled);
        if (!enabled)
        {
            // Tear down an active session right away (needs restart to re-init).
            std::lock_guard<std::mutex> lock(g_mutex);
            teardown_session_locked();
            g_status.assign("disabled by user");
        }
    }

    ImGui::Text("D3D12 device seen: %s", d3d12_seen ? "yes" : "no");
    ImGui::Text("NvOF session: %s", session ? "active" : "inactive");
    if (caps_ok)
    {
        ImGui::Text("Caps: %ux%u .. %ux%u, out grids mask 0x%X", wmin, hmin, wmax, hmax, grids);
        ImGui::TextUnformatted("(grid bit N = GRID_SIZE_N supported)");
    }
    ImGui::TextWrapped("%s", status.c_str());
}

static void register_addon_nvof()
{
    reshade::register_overlay(nullptr, draw_overlay);
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
}

static void unregister_addon_nvof()
{
    reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
}

extern "C" __declspec(dllexport) const char *NAME = "NVOF Motion";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "HW optical-flow motion vectors via NVIDIA NVOFA (phase 1b: session, no processing).";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::get_config_value(nullptr, "NVOF", "Enable", g_enabled);
        register_addon_nvof();
        break;
    case DLL_PROCESS_DETACH:
        unregister_addon_nvof();
        reshade::unregister_addon(hModule);
        // NOTE: loaded DLLs and the CUDA context are intentionally NOT torn down
        // here; driver calls inside DllMain risk loader-lock deadlocks.
        // The OS reclaims everything on process exit.
        break;
    }

    return TRUE;
}
