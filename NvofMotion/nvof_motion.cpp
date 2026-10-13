// NVOF Motion - phase 2b: vectors (execute + convert + expose).
//
// Per frame (reshade_begin_effects, game thread), with OUR OWN native D3D12
// command list (never touches ReShade's lists, no addon-event recursion):
//  - backbuffer (pre-effects image, 1 frame old by design; TAA/history pairing)
//    is copied into a CPU-visible readback buffer, signaled through our own
//    fence; completion is polled with zero timeout (never stalls - a busy
//    frame is simply skipped and self-heals next frame), then the pixels are
//    uploaded into the NvOF input CUDA buffer (HtoD) and checksummed.
//  - NvOFExecute runs on the (prev, cur) pair; SHORT2 output is downloaded
//    to CPU and converted to half + stats; a second native list uploads into
//    the R16G16F output texture, injects into TargetTexture and updates the
//    MotionVectors binding. Vectors lag ~2 frames by design.
//  - NvOF session is built lazily at the real frame size on first frame and
//    rebuilt on resize (drain via wait_idle first).
//
// Assumptions (validated at runtime, reported in overlay):
//  - backbuffer is in PRESENT state at begin_effects (post-present invariant)
//  - backbuffer format is R8G8B8A8; otherwise idle with status
//  - NVOF input GRAYSCALE8 via CPU RGBA->luma (OpenCV-faithful)
//  - injected target texture (if any) is R16G16F at grid size, sampled as SRV

#include <windows.h>
#include <d3d12.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <reshade.hpp>

#include <cuda.h> // CUDA toolkit headers only (types); no cuda.lib linkage

#include "nvOpticalFlowCommon.h" // NVIDIAOpticalFlowSDK checkout (BSD)
#include "nvOpticalFlowCuda.h"

// NOTE: flow_ptx.h (convert kernel, built by CMake) is currently unused:
// output is read as CUDEVICEPTR + converted on CPU. Kept for a future fast path.

using namespace reshade;
using namespace reshade::api;

// ---- explicit CUDA driver-API imports (no import lib needed) ----
using cuInit_fn = CUresult(CUDAAPI *)(unsigned int);
using cuDeviceGetCount_fn = CUresult(CUDAAPI *)(int *);
using cuDeviceGet_fn = CUresult(CUDAAPI *)(CUdevice *, int);
using cuDeviceGetLuid_fn = CUresult(CUDAAPI *)(char *, unsigned int *, CUdevice);
using cuCtxCreate_fn = CUresult(CUDAAPI *)(CUcontext *, unsigned int, CUdevice);
using cuCtxDestroy_fn = CUresult(CUDAAPI *)(CUcontext);
using cuDevicePrimaryCtxRetain_fn = CUresult(CUDAAPI *)(CUcontext *, CUdevice);
using cuDevicePrimaryCtxRelease_fn = CUresult(CUDAAPI *)(CUdevice);
using cuCtxSetCurrent_fn = CUresult(CUDAAPI *)(CUcontext);
using cuCtxGetCurrent_fn = CUresult(CUDAAPI *)(CUcontext *);
using cuGetErrorString_fn = CUresult(CUDAAPI *)(CUresult, const char **);
using cuStreamCreate_fn = CUresult(CUDAAPI *)(CUstream *, unsigned int);
using cuStreamDestroy_fn = CUresult(CUDAAPI *)(CUstream);
using cuStreamSynchronize_fn = CUresult(CUDAAPI *)(CUstream);
using cuMemcpy2DAsync_fn = CUresult(CUDAAPI *)(const CUDA_MEMCPY2D *, CUstream);
using cuMemAlloc_fn = CUresult(CUDAAPI *)(CUdeviceptr *, size_t);
using cuMemFree_fn = CUresult(CUDAAPI *)(CUdeviceptr);
using cuModuleLoadData_fn = CUresult(CUDAAPI *)(CUmodule *, const void *);
using cuModuleGetFunction_fn = CUresult(CUDAAPI *)(CUfunction *, CUmodule, const char *);
using cuModuleUnload_fn = CUresult(CUDAAPI *)(CUmodule);
using cuLaunchKernel_fn = CUresult(CUDAAPI *)(CUfunction, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, CUstream, void **, void **);

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
cuDevicePrimaryCtxRetain_fn p_cuDevicePrimaryCtxRetain = nullptr;
cuDevicePrimaryCtxRelease_fn p_cuDevicePrimaryCtxRelease = nullptr;
cuCtxSetCurrent_fn p_cuCtxSetCurrent = nullptr;
cuCtxGetCurrent_fn p_cuCtxGetCurrent = nullptr;
cuGetErrorString_fn p_cuGetErrorString = nullptr;
cuStreamCreate_fn p_cuStreamCreate = nullptr;
cuStreamDestroy_fn p_cuStreamDestroy = nullptr;
cuStreamSynchronize_fn p_cuStreamSynchronize = nullptr;
cuMemcpy2DAsync_fn p_cuMemcpy2DAsync = nullptr;
cuMemAlloc_fn p_cuMemAlloc = nullptr;
cuMemFree_fn p_cuMemFree = nullptr;
cuModuleLoadData_fn p_cuModuleLoadData = nullptr;
cuModuleGetFunction_fn p_cuModuleGetFunction = nullptr;
cuModuleUnload_fn p_cuModuleUnload = nullptr;
cuLaunchKernel_fn p_cuLaunchKernel = nullptr;

NvOFCreateInstance_fn p_NvOFCreateInstanceCuda = nullptr;
NV_OF_CUDA_API_FUNCTION_LIST g_of = {};
NvOFHandle g_of_handle = nullptr;

CUcontext g_cu_ctx = nullptr;
CUdevice g_cu_device = 0; // for primary-ctx release
ID3D12Device *g_owner_device = nullptr; // device owning ctx + pipeline
device *g_api_dev = nullptr;            // for create/destroy of ReShade resources
bool g_session_active = false;

unsigned int g_enabled = 1;
bool g_d3d12_seen = false;
std::string g_status = "not initialized yet";

// ---- file debug log next to the addon (bisect forensics; user attaches it) ----
HMODULE g_hmod = nullptr;
wchar_t g_dbglog_path[MAX_PATH] = {};
int g_dbg_fail_lines = 0;

void dbglog_init()
{
    if (g_hmod == nullptr || g_dbglog_path[0] != L'\0')
        return;
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(g_hmod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr || static_cast<size_t>(slash + 1 - path) + 22u >= MAX_PATH)
        return;
    *(slash + 1) = L'\0';
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"nvof_motion_debug.log");
    wcscpy_s(g_dbglog_path, MAX_PATH, path);
}

// Rare events only (bring-up, failures, first success). Own mutex, callable anywhere.
void dbglog(const char *line)
{
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    if (g_dbglog_path[0] == L'\0')
        return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, g_dbglog_path, L"a") != 0 || f == nullptr)
        return;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

// Verifies the expected ctx is actually current (stickiness check).
// Logs first 3 mismatches; callable from anywhere (own log mutex).
void check_ctx_locked(const char *where)
{
    static int logged = 0;
    if (p_cuCtxGetCurrent == nullptr || logged >= 3)
        return;
    CUcontext cur = nullptr;
    const CUresult g = p_cuCtxGetCurrent(&cur);
    if (g != CUDA_SUCCESS || cur != g_cu_ctx)
    {
        logged++;
        char buf[256] = {};
        snprintf(buf, sizeof(buf), "[dali11] CTX %s: get=%d cur=%p expected=%p", where,
                 static_cast<int>(g), cur, g_cu_ctx);
        dbglog(buf);
    }
}

// ---- frame pipeline (all guarded by g_mutex) ----
ID3D12CommandAllocator *g_cmd_alloc = nullptr;
ID3D12GraphicsCommandList *g_cmd_list = nullptr;
ID3D12Fence *g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_frame_event = nullptr; // signaled when the copy of a frame is done
CUstream g_stream = nullptr;

// Single CPU-visible readback buffer + persistent mapping + copy footprint.
ID3D12Resource *g_readback = nullptr;
void *g_readback_ptr = nullptr;
uint32_t g_readback_pitch = 0;
size_t g_readback_size = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_footprint = {};
uint64_t g_last_checksum = 0;
bool g_scene_active = false;
uint64_t g_skips = 0;

uint32_t g_frame_w = 0, g_frame_h = 0;

// OpenCV-faithful: CUDEVICEPTR buffers + driver stride (not CUarray).
NvOFGPUBufferHandle g_input_bufs[2] = { nullptr, nullptr };
CUdeviceptr g_input_ptrs[2] = { 0, 0 };
uint32_t g_input_pitch[2] = { 0, 0 };
std::vector<uint8_t> g_gray; // RGBA->luma staging (session-pitched rows)
uint32_t g_gray_pitch = 0;

// ---- phase 2b: execute output + conversion + exposure ----
NvOFGPUBufferHandle g_out_buf = nullptr;
CUdeviceptr g_out_ptr = 0; // SHORT2 flow vectors (S10.5) at grid size
uint32_t g_out_pitch = 0;
std::vector<int16_t> g_of_cpu; // int16 pairs, array->host staging for convert
NV_OF_ROI_RECT g_roi = {}; // diagnostic full-frame ROI (OF5 bisection)
CUmodule g_ptx_mod = nullptr;  // reserved for the PTX fast path (unused now)
CUfunction g_convert_fn = nullptr; // (teardown stays null-safe)
uint32_t g_grid_w = 0, g_grid_h = 0;
// R16G16F output texture (ReShade-owned) + CPU upload staging (native).
resource g_out_tex = { 0 };
resource_view g_out_srv = { 0 };
ID3D12Resource *g_upload = nullptr;
void *g_upload_ptr = nullptr;
uint32_t g_up_pitch = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_up_footprint = {};
bool g_out_ready = false; // texture left in SHADER_RESOURCE (skip entry barrier)
float g_flow_scale = 1.0f / 32.0f; // S10.5 -> px (tunable, overlay + ini)
char g_target_tex[128] = "motion_vectors_tex"; // injection target (empty = off)
bool g_target_found = false;
bool g_target_ok = false;
bool g_exec_ok = true;
float g_flow_avg = 0.0f;
float g_flow_moving_pct = 0.0f;
uint64_t g_executes = 0;

uint64_t g_frames_captured = 0;
uint64_t g_memcpys_done = 0;
int g_last_cu_code = 0;
int g_last_of_code = 0;
std::string g_last_of_error; // nvOFGetLastError text for the last execute failure
// SEH forensics: which bring-up stage crashed + exception code (0 = no crash).
int g_crash_stage = 0;
bool g_crashed_nvof = false;

// cached caps for the overlay (no CUDA calls from overlay thread)
uint32_t g_cap_wmin = 0, g_cap_wmax = 0, g_cap_hmin = 0, g_cap_hmax = 0;
uint32_t g_cap_grids = 0;
uint32_t g_cap_grids_list[8] = {};
uint32_t g_cap_grids_count = 0;
uint32_t g_cap_roi = 0;
bool g_caps_ok = false;

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

// Records the numeric code (shown in overlay) plus a readable status line.
void set_cu_status_locked(const char *what, CUresult cu)
{
    g_last_cu_code = static_cast<int>(cu);
    const std::string msg =
        std::string(what) + " (CU " + std::to_string(static_cast<int>(cu)) + ": " + cuda_error_text(cu) + ")";
    set_status_locked(msg.c_str(), reshade::log::level::warning);
}

// Round-nearest-ish float -> IEEE half bits (for R16G16F upload staging).
static uint16_t float_to_half(float v)
{
    uint32_t x = 0;
    std::memcpy(&x, &v, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 112; // rebase 127 -> 15
    uint32_t mant = x & 0x7FFFFFu;
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7BFFu); // overflow clamp (incl. inf/nan)
    if (exp <= 0)
    {
        if (exp < -10)
            return static_cast<uint16_t>(sign); // underflow to zero
        mant |= 0x800000u; // hidden leading 1
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t half = mant >> shift;
        half += (mant >> (shift - 1)) & 1u; // round
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    half += (mant >> 12) & 1u; // round
    return static_cast<uint16_t>(sign | half);
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

bool resolve_cuda_symbols_locked()
{
    if (g_cuda_dll == nullptr)
    {
        g_cuda_dll = LoadLibraryW(L"nvcuda.dll");
        if (g_cuda_dll == nullptr)
        {
            set_status_locked("nvcuda.dll NOT found - no CUDA driver, staying idle", reshade::log::level::warning);
            return false;
        }
    }
#define NVOF_RESOLVE(var, type, prim, fb)                                     \
    var = reinterpret_cast<type>(load_sym(g_cuda_dll, prim, fb));             \
    if (var == nullptr)                                                       \
    {                                                                         \
        set_status_locked("nvcuda.dll present but " prim " missing", reshade::log::level::warning); \
        return false;                                                         \
    }
    NVOF_RESOLVE(p_cuInit, cuInit_fn, "cuInit", nullptr);
    NVOF_RESOLVE(p_cuDeviceGetCount, cuDeviceGetCount_fn, "cuDeviceGetCount", nullptr);
    NVOF_RESOLVE(p_cuDeviceGet, cuDeviceGet_fn, "cuDeviceGet", nullptr);
    NVOF_RESOLVE(p_cuDeviceGetLuid, cuDeviceGetLuid_fn, "cuDeviceGetLuid", nullptr);
    NVOF_RESOLVE(p_cuCtxCreate, cuCtxCreate_fn, "cuCtxCreate", "cuCtxCreate_v2");
    NVOF_RESOLVE(p_cuCtxDestroy, cuCtxDestroy_fn, "cuCtxDestroy", "cuCtxDestroy_v2");
    NVOF_RESOLVE(p_cuDevicePrimaryCtxRetain, cuDevicePrimaryCtxRetain_fn, "cuDevicePrimaryCtxRetain",
                 nullptr);
    NVOF_RESOLVE(p_cuDevicePrimaryCtxRelease, cuDevicePrimaryCtxRelease_fn, "cuDevicePrimaryCtxRelease",
                 nullptr);
    NVOF_RESOLVE(p_cuCtxSetCurrent, cuCtxSetCurrent_fn, "cuCtxSetCurrent", nullptr);
    NVOF_RESOLVE(p_cuCtxGetCurrent, cuCtxGetCurrent_fn, "cuCtxGetCurrent", nullptr);
    NVOF_RESOLVE(p_cuGetErrorString, cuGetErrorString_fn, "cuGetErrorString", nullptr);
    NVOF_RESOLVE(p_cuStreamCreate, cuStreamCreate_fn, "cuStreamCreate", nullptr);
    NVOF_RESOLVE(p_cuStreamDestroy, cuStreamDestroy_fn, "cuStreamDestroy", nullptr);
    NVOF_RESOLVE(p_cuStreamSynchronize, cuStreamSynchronize_fn, "cuStreamSynchronize", nullptr);
    NVOF_RESOLVE(p_cuMemcpy2DAsync, cuMemcpy2DAsync_fn, "cuMemcpy2DAsync", "cuMemcpy2DAsync_v2");
    NVOF_RESOLVE(p_cuMemAlloc, cuMemAlloc_fn, "cuMemAlloc", "cuMemAlloc_v2");
    NVOF_RESOLVE(p_cuMemFree, cuMemFree_fn, "cuMemFree", "cuMemFree_v2");
    NVOF_RESOLVE(p_cuModuleLoadData, cuModuleLoadData_fn, "cuModuleLoadData", nullptr);
    NVOF_RESOLVE(p_cuModuleGetFunction, cuModuleGetFunction_fn, "cuModuleGetFunction", nullptr);
    NVOF_RESOLVE(p_cuModuleUnload, cuModuleUnload_fn, "cuModuleUnload", nullptr);
    NVOF_RESOLVE(p_cuLaunchKernel, cuLaunchKernel_fn, "cuLaunchKernel", nullptr);
#undef NVOF_RESOLVE
    return true;
}

// Destroys NvOF session + input buffers, keeps CUDA context. ctx must be current.
void destroy_of_locked()
{
    for (int i = 0; i < 2; ++i)
    {
        if (g_input_bufs[i] != nullptr && g_of.nvOFDestroyGPUBufferCuda != nullptr)
            g_of.nvOFDestroyGPUBufferCuda(g_input_bufs[i]);
        g_input_bufs[i] = nullptr;
        g_input_ptrs[i] = 0;
        g_input_pitch[i] = 0;
    }
    if (g_out_buf != nullptr && g_of.nvOFDestroyGPUBufferCuda != nullptr)
        g_of.nvOFDestroyGPUBufferCuda(g_out_buf);
    g_out_buf = nullptr;
    g_out_ptr = 0;
    g_out_pitch = 0;
    if (g_of_handle != nullptr && g_of.nvOFDestroy != nullptr)
    {
        g_of.nvOFDestroy(g_of_handle);
        g_of_handle = nullptr;
    }
    g_session_active = false;
    g_caps_ok = false;
}

// Drops readback buffer + session/inputs (+ linear scratch). Keeps ctx,
// table, DLLs and size-independent objects (list/alloc/fence/event/stream).
// Output texture/SRV are destroyed only when gpu_idle (GPU may sample them).
// Manages ctx itself.
void teardown_pipeline_locked(bool gpu_idle)
{
    if (g_cu_ctx != nullptr && p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(g_cu_ctx);

    destroy_of_locked();

    g_of_cpu.clear();
    g_of_cpu.shrink_to_fit();
    g_gray.clear();
    g_gray.shrink_to_fit();
    g_gray_pitch = 0;
    if (g_readback != nullptr)
    {
        const D3D12_RANGE written = { 0, 0 }; // CPU never writes
        g_readback->Unmap(0, &written);
        g_readback->Release();
        g_readback = nullptr;
    }
    g_readback_ptr = nullptr;
    g_readback_pitch = 0;
    g_readback_size = 0;
    if (g_upload != nullptr)
    {
        g_upload->Release(); // native refcount: safe anytime, GPU retains
        g_upload = nullptr;
    }
    g_upload_ptr = nullptr;
    if (gpu_idle && g_api_dev != nullptr)
    {
        if (g_out_srv.handle != 0)
            g_api_dev->destroy_resource_view(g_out_srv);
        g_out_srv = { 0 };
        if (g_out_tex.handle != 0)
            g_api_dev->destroy_resource(g_out_tex);
        g_out_tex = { 0 };
    }
    g_out_ready = false;
    g_grid_w = g_grid_h = 0;
    g_frame_w = g_frame_h = 0;

    if (p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(nullptr);
}

// Full teardown incl. CUDA context. Safe to call with partial state.
void teardown_all_locked()
{
    if (g_cu_ctx != nullptr && p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(g_cu_ctx);

    teardown_pipeline_locked(false); // no drain here: leak cb texture

    // teardown_pipeline_locked detached; re-attach for the work below.
    if (g_cu_ctx != nullptr && p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(g_cu_ctx);

    if (g_ptx_mod != nullptr && p_cuModuleUnload != nullptr)
        p_cuModuleUnload(g_ptx_mod);
    g_ptx_mod = nullptr;
    g_convert_fn = nullptr;
    if (g_stream != nullptr)
    {
        if (p_cuStreamSynchronize != nullptr)
            p_cuStreamSynchronize(g_stream);
        if (p_cuStreamDestroy != nullptr)
            p_cuStreamDestroy(g_stream);
        g_stream = nullptr;
    }
    if (g_cmd_list != nullptr)
    {
        g_cmd_list->Release();
        g_cmd_list = nullptr;
    }
    if (g_cmd_alloc != nullptr)
    {
        g_cmd_alloc->Release();
        g_cmd_alloc = nullptr;
    }
    if (g_fence != nullptr)
    {
        g_fence->Release();
        g_fence = nullptr;
    }
    if (g_frame_event != nullptr)
    {
        CloseHandle(g_frame_event);
        g_frame_event = nullptr;
    }
    g_fence_value = 0;

    if (g_cu_ctx != nullptr)
    {
        if (p_cuCtxSetCurrent != nullptr)
            p_cuCtxSetCurrent(nullptr);
        if (p_cuDevicePrimaryCtxRelease != nullptr)
            p_cuDevicePrimaryCtxRelease(g_cu_device);
        g_cu_ctx = nullptr;
        g_cu_device = 0;
    }
    g_owner_device = nullptr;
    g_api_dev = nullptr;
}

// POD-only NvOF bring-up with SEH guard: any driver crash inside becomes a
// reported status (stage + exception code) instead of a dead process.
// Stages: 1 = create handle, 2 = init, 3/4 = input buffers, 5 = output.
// Returns false on error or exception. On exception, handles are deliberately
// leaked (never touch the driver again on that path); g_crashed_nvof is set.
// NOTE: no unwindable locals in here (C2712) - POD and globals only.
bool create_of_nvof_locked(uint32_t w, uint32_t h)
{
    g_crash_stage = 0;
    g_crashed_nvof = false;
    __try
    {
        g_crash_stage = 1;
        NvOFHandle hOf = nullptr;
        NV_OF_STATUS st = g_of.nvCreateOpticalFlowCuda(g_cu_ctx, &hOf);
        if (st != NV_OF_SUCCESS || hOf == nullptr)
        {
            g_last_of_code = static_cast<int>(st);
            return false;
        }
        g_of_handle = hOf;

        g_crash_stage = 2;
        NV_OF_INIT_PARAMS init = {};
        init.width = w;
        init.height = h;
        init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        // OpenCV-faithful: hintGridSize UNDEFINED when external hints off
        // (spec: field only considered when enableExternalHints set).
        init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = NV_OF_PERF_LEVEL_FAST;
        init.enableExternalHints = NV_OF_FALSE;
        init.enableOutputCost = NV_OF_FALSE; // v1: pure vectors, no cost plane
        init.hPrivData = nullptr;

        st = g_of.nvOFInit(hOf, &init);
        if (st != NV_OF_SUCCESS)
        {
            g_last_of_code = static_cast<int>(st);
            return false;
        }

        // Bisect streams: skip SetIO entirely (driver default streams =
        // OpenCV null-stream path). Tests whether our stream handle is poison.
        // (OpenCV order would be: SetIO AFTER init.)
        (void)g_stream;

        for (int i = 0; i < 2; ++i)
        {
            g_crash_stage = 3 + i;
            NV_OF_BUFFER_DESCRIPTOR desc = {};
            desc.width = w;
            desc.height = h;
            desc.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
            desc.bufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8; // OpenCV-faithful
            st = g_of.nvOFCreateGPUBufferCuda(hOf, &desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
                                               &g_input_bufs[i]);
            if (st != NV_OF_SUCCESS || g_input_bufs[i] == nullptr)
            {
                g_last_of_code = static_cast<int>(st);
                return false;
            }
            g_input_ptrs[i] = g_of.nvOFGPUBufferGetCUdeviceptr(g_input_bufs[i]);
            if (g_input_ptrs[i] == 0)
                return false;
            // Driver pitch (OpenCV order); tight fallback (1 byte/px).
            g_input_pitch[i] = w;
            if (g_of.nvOFGPUBufferGetStrideInfo != nullptr)
            {
                NV_OF_CUDA_BUFFER_STRIDE_INFO si = {};
                if (g_of.nvOFGPUBufferGetStrideInfo(g_input_bufs[i], &si) == NV_OF_SUCCESS &&
                    si.strideInfo[0].strideXInBytes != 0)
                    g_input_pitch[i] = si.strideInfo[0].strideXInBytes;
            }
        }
        g_crash_stage = 5;
        NV_OF_BUFFER_DESCRIPTOR odesc = {};
        odesc.width = (w + 3) / 4; // ceil (OpenCV order)
        odesc.height = (h + 3) / 4;
        odesc.bufferUsage = NV_OF_BUFFER_USAGE_OUTPUT;
        odesc.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;
        st = g_of.nvOFCreateGPUBufferCuda(hOf, &odesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &g_out_buf);
        if (st != NV_OF_SUCCESS || g_out_buf == nullptr)
        {
            g_last_of_code = static_cast<int>(st);
            return false;
        }
        g_out_ptr = g_of.nvOFGPUBufferGetCUdeviceptr(g_out_buf);
        if (g_out_ptr == 0)
            return false;
        g_out_pitch = ((w + 3) / 4) * sizeof(int16_t) * 2; // tight fallback
        if (g_of.nvOFGPUBufferGetStrideInfo != nullptr)
        {
            NV_OF_CUDA_BUFFER_STRIDE_INFO osi = {};
            if (g_of.nvOFGPUBufferGetStrideInfo(g_out_buf, &osi) == NV_OF_SUCCESS &&
                osi.strideInfo[0].strideXInBytes != 0)
                g_out_pitch = osi.strideInfo[0].strideXInBytes;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_last_of_code = static_cast<int>(GetExceptionCode());
        g_crashed_nvof = true;
        return false;
    }
}

// (Re)builds NvOF session + buffers at W x H. ctx must be current.
bool create_of_locked(uint32_t w, uint32_t h)
{
    destroy_of_locked();

    if (!create_of_nvof_locked(w, h))
    {
        if (g_crashed_nvof)
        {
            // Driver crashed mid-bring-up: leak handles on purpose, report it.
            g_of_handle = nullptr;
            g_input_bufs[0] = nullptr;
            g_input_bufs[1] = nullptr;
            g_input_ptrs[0] = 0;
            g_input_ptrs[1] = 0;
            g_input_pitch[0] = 0;
            g_input_pitch[1] = 0;
            g_out_buf = nullptr;
            g_out_ptr = 0;
            g_out_pitch = 0;
            char code[32] = {};
            snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned int>(g_last_of_code));
            const std::string msg = std::string("[NVOF Motion] DRIVER CRASH in bring-up (stage ") +
                std::to_string(g_crash_stage) + ", code " + code + ") - staying idle";
            set_status_locked(msg.c_str(), reshade::log::level::error);
        }
        else
        {
            destroy_of_locked();
        }
        return false;
    }

    auto query_cap = [&](NV_OF_CAPS cap, uint32_t &out) {
        uint32_t val = 0;
        uint32_t size = sizeof(val);
        if (g_of.nvOFGetCaps(g_of_handle, cap, &val, &size) == NV_OF_SUCCESS)
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
    // Two-stage caps (OpenCV order): real supported-grids array + ROI support.
    if (g_of.nvOFGetCaps != nullptr)
    {
        uint32_t n = 0;
        if (g_of.nvOFGetCaps(g_of_handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &n) ==
                NV_OF_SUCCESS &&
            n > 0 && n <= 8)
        {
            uint32_t vals[8] = {};
            uint32_t m = n;
            if (g_of.nvOFGetCaps(g_of_handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, vals, &m) ==
                NV_OF_SUCCESS)
            {
                g_cap_grids_count = (m <= 8) ? m : 8;
                for (uint32_t i = 0; i < g_cap_grids_count; ++i)
                    g_cap_grids_list[i] = vals[i];
            }
        }
        uint32_t roi = 0, rsz = sizeof(roi);
        if (g_of.nvOFGetCaps(g_of_handle, NV_OF_CAPS_SUPPORT_ROI, &roi, &rsz) == NV_OF_SUCCESS)
            g_cap_roi = roi;
    }

    // int16 pairs staging for the array->host download (ceil, OpenCV order).
    g_grid_w = (w + 3) / 4;
    g_grid_h = (h + 3) / 4;
    g_of_cpu.assign(static_cast<size_t>(g_grid_w) * g_grid_h * 2, 0);
    g_gray.assign(static_cast<size_t>(w) * h, 0); // luma staging, session-pitched
    g_gray_pitch = w;

    {
        const std::string msg =
            "[NVOF Motion] session init ok (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        reshade::log::message(reshade::log::level::info, msg.c_str());
        char buf[768] = {};
        snprintf(buf, sizeof(buf),
                 "[dali11] session %ux%u init(grid4,hintUndef,noExt,noCost,FAST,OF,inGRAY8) caps %u-%u x %u-%u "
                 "grids[%u]{%u %u %u %u %u %u %u %u} roi %u | "
                 "in0=0x%llX pitch=%u in1=0x%llX pitch=%u | out=0x%llX pitch=%u outgrid=%ux%u",
                 w, h, g_cap_wmin, g_cap_wmax, g_cap_hmin, g_cap_hmax, g_cap_grids_count,
                 g_cap_grids_list[0], g_cap_grids_list[1], g_cap_grids_list[2], g_cap_grids_list[3],
                 g_cap_grids_list[4], g_cap_grids_list[5], g_cap_grids_list[6], g_cap_grids_list[7],
                 g_cap_roi, static_cast<unsigned long long>(g_input_ptrs[0]), g_input_pitch[0],
                 static_cast<unsigned long long>(g_input_ptrs[1]), g_input_pitch[1],
                 static_cast<unsigned long long>(g_out_ptr), g_out_pitch, g_grid_w, g_grid_h);
        dbglog(buf);
    }

    g_last_of_error.clear();
    g_exec_ok = true;
    g_session_active = true;
    return true;
}

// (Re)builds the whole frame pipeline at W x H (readback + session).
// Caller must have drained the GPU first (wait_idle) when rebuilding.
bool build_pipeline_locked(device *dev, ID3D12Device *d3d12, uint32_t w, uint32_t h)
{
    if (p_cuCtxSetCurrent == nullptr || g_cu_ctx == nullptr)
        return false;
    {
        const CUresult sc = p_cuCtxSetCurrent(g_cu_ctx);
        if (sc != CUDA_SUCCESS)
        {
            set_cu_status_locked("build set-current failed", sc);
            return false;
        }
    }
    check_ctx_locked("build");

    // On any failure below, the caller runs teardown_pipeline_locked(),
    // which safely drops partial state.
    // NvOF session + input dims are aligned up (HW grid friendliness);
    // readback + memcpy stay at the real frame size (subregion copy).
    // Bisect NVOFA HW alignment (3440 % 32 = 16): session + buffers padded
    // to 128x32 (ROI-grade); upload stays a real-size subregion.
    const uint32_t sw = (w + 127u) & ~127u;
    const uint32_t sh = (h + 31u) & ~31u;
    {
        char buf[128] = {};
        snprintf(buf, sizeof(buf), "[dali11] build req %ux%u -> session %ux%u", w, h, sw, sh);
        dbglog(buf);
    }
    bool ok = false;
    do // single-pass for linear error handling
    {
        if (g_cmd_list == nullptr)
        {
            if (FAILED(d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc))) ||
                FAILED(d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc, nullptr,
                                                IID_PPV_ARGS(&g_cmd_list))) ||
                FAILED(g_cmd_list->Close()))
            {
                set_status_locked("native command list creation failed", reshade::log::level::warning);
                break;
            }
        }
        reshade::log::message(reshade::log::level::info, "[NVOF Motion] build: cmd list ok");
        if (g_fence == nullptr)
        {
            if (FAILED(d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
            {
                set_status_locked("CreateFence failed", reshade::log::level::warning);
                break;
            }
            g_frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_frame_event == nullptr)
            {
                set_status_locked("CreateEvent failed", reshade::log::level::warning);
                break;
            }
        }
        reshade::log::message(reshade::log::level::info, "[NVOF Motion] build: fence+event ok");
        if (g_stream == nullptr)
        {
            const CUresult src = p_cuStreamCreate(&g_stream, 0);
            if (src != CUDA_SUCCESS || g_stream == nullptr)
            {
                set_cu_status_locked("cuStreamCreate failed", src);
                break;
            }
        }
        reshade::log::message(reshade::log::level::info, "[NVOF Motion] build: stream ok");
        // Readback buffer for one frame (placed footprint of an RGBA8 image).
        {
            D3D12_RESOURCE_DESC img_desc = {};
            img_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            img_desc.Width = w;
            img_desc.Height = h;
            img_desc.DepthOrArraySize = 1;
            img_desc.MipLevels = 1;
            img_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            img_desc.SampleDesc.Count = 1;
            img_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
            UINT64 total_bytes = 0;
            d3d12->GetCopyableFootprints(&img_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
            if (total_bytes == 0 || footprint.Footprint.RowPitch == 0)
            {
                set_status_locked("GetCopyableFootprints failed", reshade::log::level::warning);
                break;
            }

            D3D12_RESOURCE_DESC buf_desc = {};
            buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buf_desc.Width = total_bytes;
            buf_desc.Height = 1;
            buf_desc.DepthOrArraySize = 1;
            buf_desc.MipLevels = 1;
            buf_desc.Format = DXGI_FORMAT_UNKNOWN;
            buf_desc.SampleDesc.Count = 1;
            buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

            ID3D12Resource *readback = nullptr;
            if (FAILED(d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback))) ||
                readback == nullptr)
            {
                set_status_locked("readback CreateCommittedResource failed", reshade::log::level::warning);
                break;
            }
            void *mapped_ptr = nullptr;
            if (FAILED(readback->Map(0, nullptr, &mapped_ptr)) || mapped_ptr == nullptr)
            {
                readback->Release();
                set_status_locked("readback Map failed", reshade::log::level::warning);
                break;
            }

            g_readback = readback;
            g_readback_ptr = mapped_ptr;
            g_readback_pitch = footprint.Footprint.RowPitch;
            g_readback_size = static_cast<size_t>(total_bytes);
            g_footprint = footprint;
        }

        reshade::log::message(reshade::log::level::info, "[NVOF Motion] build: readback ok");
        // R16G16F output texture (ReShade-owned) + CPU upload staging (native).
        {
            const uint32_t gw = (sw + 3) / 4, gh = (sh + 3) / 4;
            const resource_desc tdesc(gw, gh, 1, 1, format::r16g16_float, 1, memory_heap::default_,
                                      resource_usage::copy_dest | resource_usage::shader_resource);
            resource out_tex = { 0 };
            if (!dev->create_resource(tdesc, nullptr, resource_usage::copy_dest, &out_tex) ||
                out_tex.handle == 0)
            {
                set_status_locked("output create_resource failed", reshade::log::level::warning);
                break;
            }
            resource_view srv = { 0 };
            if (!dev->create_resource_view(out_tex, resource_usage::shader_resource,
                                           resource_view_desc(format::r16g16_float), &srv) ||
                srv.handle == 0)
            {
                dev->destroy_resource(out_tex);
                set_status_locked("output create_resource_view failed", reshade::log::level::warning);
                break;
            }

            D3D12_RESOURCE_DESC up_img = {};
            up_img.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            up_img.Width = gw;
            up_img.Height = gh;
            up_img.DepthOrArraySize = 1;
            up_img.MipLevels = 1;
            up_img.Format = DXGI_FORMAT_R16G16_FLOAT;
            up_img.SampleDesc.Count = 1;
            up_img.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT up_foot = {};
            UINT64 up_bytes = 0;
            d3d12->GetCopyableFootprints(&up_img, 0, 1, 0, &up_foot, nullptr, nullptr, &up_bytes);
            if (up_bytes == 0 || up_foot.Footprint.RowPitch == 0)
            {
                dev->destroy_resource_view(srv);
                dev->destroy_resource(out_tex);
                set_status_locked("GetCopyableFootprints (upload) failed", reshade::log::level::warning);
                break;
            }

            D3D12_RESOURCE_DESC up_buf = {};
            up_buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            up_buf.Width = up_bytes;
            up_buf.Height = 1;
            up_buf.DepthOrArraySize = 1;
            up_buf.MipLevels = 1;
            up_buf.Format = DXGI_FORMAT_UNKNOWN;
            up_buf.SampleDesc.Count = 1;
            up_buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            D3D12_HEAP_PROPERTIES up_heap = {};
            up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            up_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            up_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

            ID3D12Resource *staging = nullptr;
            if (FAILED(d3d12->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &up_buf,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&staging))) ||
                staging == nullptr)
            {
                dev->destroy_resource_view(srv);
                dev->destroy_resource(out_tex);
                set_status_locked("upload CreateCommittedResource failed", reshade::log::level::warning);
                break;
            }
            void *staging_ptr = nullptr;
            if (FAILED(staging->Map(0, nullptr, &staging_ptr)) || staging_ptr == nullptr)
            {
                staging->Release();
                dev->destroy_resource_view(srv);
                dev->destroy_resource(out_tex);
                set_status_locked("upload Map failed", reshade::log::level::warning);
                break;
            }

            g_out_tex = out_tex;
            g_out_srv = srv;
            g_upload = staging;
            g_upload_ptr = staging_ptr;
            g_up_pitch = up_foot.Footprint.RowPitch;
            g_up_footprint = up_foot;
            g_out_ready = false; // fresh COPY_DEST: skip entry barrier on first upload
        }

        reshade::log::message(reshade::log::level::info, "[NVOF Motion] build: output ok");
        if (!create_of_locked(sw, sh))
            break;

        g_frame_w = w;
        g_frame_h = h;
        g_roi.start_x = 0;
        g_roi.start_y = 0;
        g_roi.width = (w / 128) * 128;
        g_roi.height = (h / 32) * 32;
        ok = true;
    } while (false);

    if (p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(nullptr);
    return ok;
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

    ID3D12Device *d3d12 = reinterpret_cast<ID3D12Device *>(device->get_native());
    if (d3d12 == nullptr)
    {
        set_status_locked("D3D12 device has no native handle", reshade::log::level::warning);
        return;
    }
    if (g_owner_device == d3d12 && g_cu_ctx != nullptr)
        return; // already serving this device (session comes lazily)
    teardown_all_locked(); // clean start (also migrates multi-GPU)

    if (!resolve_cuda_symbols_locked())
        return;

    CUresult cu = p_cuInit(0);
    if (cu != CUDA_SUCCESS)
    {
        set_status_locked(cuda_error_text(cu).c_str(), reshade::log::level::warning);
        return;
    }

    // Match CUDA device to the game's D3D12 adapter (LUID).
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

    // Primary context (OpenCV-runtime/DALI convention): both proven stacks run
    // NvOF on the retained primary, never on a fresh cuCtxCreate context.
    CUcontext ctx = nullptr;
    cu = p_cuDevicePrimaryCtxRetain(&ctx, static_cast<CUdevice>(matched));
    if (cu != CUDA_SUCCESS || ctx == nullptr)
    {
        set_status_locked(cuda_error_text(cu).c_str(), reshade::log::level::warning);
        return;
    }
    if (p_cuCtxSetCurrent(ctx) != CUDA_SUCCESS)
    {
        set_status_locked("primary ctx retained but set-current failed", reshade::log::level::warning);
        p_cuDevicePrimaryCtxRelease(static_cast<CUdevice>(matched));
        return;
    }
    g_cu_ctx = ctx;
    g_cu_device = static_cast<CUdevice>(matched);

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
        p_cuCtxSetCurrent(nullptr);
        return;
    }
    g_of = {};
    {
        const NV_OF_STATUS st = p_NvOFCreateInstanceCuda(NV_OF_API_VERSION, &g_of);
        if (st != NV_OF_SUCCESS || g_of.nvCreateOpticalFlowCuda == nullptr || g_of.nvOFInit == nullptr ||
            g_of.nvOFGetCaps == nullptr || g_of.nvOFDestroy == nullptr ||
            g_of.nvOFCreateGPUBufferCuda == nullptr || g_of.nvOFGPUBufferGetCUarray == nullptr ||
            g_of.nvOFDestroyGPUBufferCuda == nullptr || g_of.nvOFExecute == nullptr ||
            g_of.nvOFGPUBufferGetCUdeviceptr == nullptr || g_of.nvOFSetIOCudaStreams == nullptr)
        {
            g_last_of_code = static_cast<int>(st);
            set_status_locked("NvOFAPICreateInstanceCuda failed or incomplete", reshade::log::level::warning);
            p_cuCtxSetCurrent(nullptr);
            return;
        }
    }

    g_owner_device = d3d12;
    g_api_dev = device;
    p_cuCtxSetCurrent(nullptr);
    set_status_locked("driver ready, waiting for first frame (session builds lazily)",
                      reshade::log::level::info);
}

static void on_destroy_device(device *device)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_owner_device != nullptr &&
        device->get_native() == reinterpret_cast<uint64_t>(g_owner_device))
    {
        teardown_all_locked();
        g_status.assign("torn down (device destroyed)");
    }
}

static void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
{
    if (g_enabled == 0)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cu_ctx == nullptr || g_owner_device == nullptr)
        return; // init_device did not complete (or non-D3D12)

    device *dev = runtime->get_device();
    ID3D12Device *d3d12 = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if (d3d12 == nullptr || d3d12 != g_owner_device)
        return;
    const resource bb = runtime->get_current_back_buffer();
    if (bb.handle == 0)
        return;
    const resource_desc bb_desc = dev->get_resource_desc(bb);
    if (bb_desc.type != resource_type::texture_2d || bb_desc.texture.width == 0 || bb_desc.texture.height == 0)
        return;
    if (bb_desc.texture.format != format::r8g8b8a8_unorm)
    {
        if (g_session_active || g_readback != nullptr)
        {
            // Pipeline exists but format is unsupported: drop it, keep ctx
            // (teardown manages the context itself) for automatic recovery.
            teardown_pipeline_locked(false); // no drain here: leak cb texture
            set_status_locked("unsupported backbuffer format (need R8G8B8A8), idle", reshade::log::level::warning);
        }
        return;
    }
    const uint32_t w = bb_desc.texture.width;
    const uint32_t h = bb_desc.texture.height;

    command_queue *queue = runtime->get_command_queue();
    ID3D12CommandQueue *native_queue =
        queue != nullptr ? reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()) : nullptr;
    if (native_queue == nullptr)
        return;

    if (!g_session_active || g_readback == nullptr || w != g_frame_w || h != g_frame_h)
    {
        // First frame or resize/format change: rebuild pipeline.
        // Drain the GPU only when live objects exist (first build has nothing
        // in flight; wait_idle on a fresh device risks stall/deadlock).
        // (teardown/build manage the CUDA context themselves.)
        if (g_session_active || g_readback != nullptr)
            queue->wait_idle();
        teardown_pipeline_locked(true); // GPU drained above: destroy cb texture
        const bool built = build_pipeline_locked(dev, d3d12, w, h);
        if (!built)
        {
            teardown_pipeline_locked(true); // drop partial state, retry next frame
            return;                         // status already set by the failing step
        }
        set_status_locked("capture pipeline active", reshade::log::level::info);
    }

    // ---- per-frame: copy bb -> readback, fence, poll, HtoD memcpy ----
    ID3D12Resource *bb_res = reinterpret_cast<ID3D12Resource *>(bb.handle);

    if (FAILED(g_cmd_alloc->Reset()) || FAILED(g_cmd_list->Reset(g_cmd_alloc, nullptr)))
    {
        set_status_locked("native command list reset failed", reshade::log::level::warning);
        return;
    }
    {
        D3D12_RESOURCE_BARRIER to_copy = {};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = bb_res;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cmd_list->ResourceBarrier(1, &to_copy);
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
        dst_loc.pResource = g_readback;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint = g_footprint;
        D3D12_TEXTURE_COPY_LOCATION src_loc = {};
        src_loc.pResource = bb_res;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;
        g_cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    }
    {
        D3D12_RESOURCE_BARRIER to_present = {};
        to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Transition.pResource = bb_res;
        to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cmd_list->ResourceBarrier(1, &to_present);
    }
    if (FAILED(g_cmd_list->Close()))
    {
        set_status_locked("native command list close failed", reshade::log::level::warning);
        return;
    }
    ID3D12CommandList *lists[] = { g_cmd_list };
    native_queue->ExecuteCommandLists(1, lists);
    native_queue->Signal(g_fence, ++g_fence_value);

    // Non-blocking completion poll: if the copy is not done yet, skip this
    // frame (vectors stay one frame older, self-heals next frame). Never stalls.
    g_fence->SetEventOnCompletion(g_fence_value, g_frame_event);
    if (WaitForSingleObject(g_frame_event, 0) != WAIT_OBJECT_0)
    {
        g_skips++;
        return;
    }

    if (p_cuCtxSetCurrent != nullptr)
    {
        const CUresult sc = p_cuCtxSetCurrent(g_cu_ctx);
        if (sc != CUDA_SUCCESS)
        {
            g_last_cu_code = static_cast<int>(sc);
            set_status_locked(cuda_error_text(sc).c_str(), reshade::log::level::warning);
            return;
        }
    }
    check_ctx_locked("frame");
    // RGBA -> luma staging (OpenCV GRAYSCALE8 path); gray rows are session-pitched.
    CUresult cu = CUDA_SUCCESS;
    if (g_gray_pitch == 0 || g_gray.size() < static_cast<size_t>(g_gray_pitch) * h)
    {
        cu = CUDA_ERROR_INVALID_VALUE; // session/gray mismatch -> skip frame (loud)
    }
    else
    {
        uint8_t *gd = g_gray.data();
        const uint8_t *rb = static_cast<const uint8_t *>(g_readback_ptr);
        for (uint32_t y = 0; y < h; ++y)
        {
            uint8_t *drow = gd + static_cast<size_t>(y) * g_gray_pitch;
            const uint8_t *srow = rb + static_cast<size_t>(y) * g_readback_pitch;
            for (uint32_t x = 0; x < w; ++x)
                drow[x] = static_cast<uint8_t>((77u * srow[4 * x] + 150u * srow[4 * x + 1] +
                                                29u * srow[4 * x + 2]) >>
                                               8);
        }
    }
    CUDA_MEMCPY2D m = {};
    m.srcXInBytes = 0;
    m.srcY = 0;
    m.srcMemoryType = CU_MEMORYTYPE_HOST;
    m.srcHost = g_gray.data();
    m.srcPitch = g_gray_pitch;
    m.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    m.dstDevice = g_input_ptrs[g_frames_captured % 2];
    m.dstPitch = g_input_pitch[g_frames_captured % 2];
    m.WidthInBytes = static_cast<size_t>(w);
    m.Height = h;
    if (cu == CUDA_SUCCESS)
        cu = p_cuMemcpy2DAsync(&m, g_stream);
    if (cu == CUDA_SUCCESS)
        cu = p_cuStreamSynchronize(g_stream);
    // Cheap liveness proof: sampled checksum of the captured pixels.
    uint64_t sum = 0;
    if (cu == CUDA_SUCCESS)
    {
        const uint8_t *base = static_cast<const uint8_t *>(g_readback_ptr);
        for (uint32_t y = 0; y < h; y += 8)
        {
            const uint8_t *row = base + static_cast<size_t>(y) * g_readback_pitch;
            for (uint32_t x = 0; x < w * 4; x += 64)
                sum += row[x];
        }
    }

    // ---- NvOF execute on pair (prev, cur), convert on CPU, stats ----
    // Runs synchronously (execute blocks by API design); only when both
    // input buffers hold a frame (from the 2nd captured frame on).
    bool have_vectors = false;
    if (cu == CUDA_SUCCESS && g_frames_captured >= 1 && g_out_buf != nullptr && g_out_ptr != 0 &&
        !g_of_cpu.empty())
    {
        const int just_filled = static_cast<int>(g_frames_captured % 2);
        // DALI-faithful: zero-init only (no ROI, no hints flags).
        NV_OF_EXECUTE_INPUT_PARAMS in_params = {};
        // OpenCV convention (calc(current, previous)): input = current frame.
        in_params.inputFrame = g_input_bufs[just_filled];
        in_params.referenceFrame = g_input_bufs[1 - just_filled];
        // Guide default (temporal enabled); converges to the proven OpenCV
        // recipe (gray8 + devptr + no-SetIO + cur/prev + temporal on).
        in_params.disableTemporalHints = NV_OF_FALSE;
        NV_OF_EXECUTE_OUTPUT_PARAMS out_params = {};
        out_params.outputBuffer = g_out_buf;
        out_params.outputCostBuffer = nullptr; // enableOutputCost=FALSE (OpenCV-faithful)
        out_params.hPrivData = nullptr;
        const NV_OF_STATUS ost = g_of.nvOFExecute(g_of_handle, &in_params, &out_params);
        if (ost != NV_OF_SUCCESS)
        {
            g_last_of_code = static_cast<int>(ost);
            // Driver detail string (once per failure burst; overlay shows it).
            if (g_of.nvOFGetLastError != nullptr)
            {
                char err[256] = {};
                uint32_t errsz = sizeof(err);
                if (g_of.nvOFGetLastError(g_of_handle, err, &errsz) == NV_OF_SUCCESS && err[0] != '\0')
                    g_last_of_error.assign(err);
            }
            if (g_dbg_fail_lines < 5)
            {
                g_dbg_fail_lines++;
                char buf[512] = {};
                snprintf(buf, sizeof(buf),
                         "[dali11] exec FAIL #%d: OF %d detail='%s' inHandle=%p refHandle=%p outHandle=%p "
                         "frames=%llu",
                         g_dbg_fail_lines, static_cast<int>(ost), g_last_of_error.c_str(),
                         in_params.inputFrame, in_params.referenceFrame, out_params.outputBuffer,
                         static_cast<unsigned long long>(g_frames_captured));
                dbglog(buf);
            }
            if (g_exec_ok)
            {
                g_exec_ok = false;
                std::string msg = std::string("nvOFExecute failed") +
                    (g_last_of_error.empty() ? "" : std::string(" : ") + g_last_of_error);
                set_status_locked(msg.c_str(), reshade::log::level::warning);
            }
        }
        else
        {
            g_exec_ok = true;
            // SHORT2 vectors -> host (int16 pairs); float convert happens on CPU.
            CUDA_MEMCPY2D d = {};
            d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            d.srcDevice = g_out_ptr;
            d.srcPitch = g_out_pitch;
            d.dstMemoryType = CU_MEMORYTYPE_HOST;
            d.dstHost = g_of_cpu.data();
            d.dstPitch = static_cast<size_t>(g_grid_w) * sizeof(int16_t) * 2;
            d.WidthInBytes = static_cast<size_t>(g_grid_w) * sizeof(int16_t) * 2;
            d.Height = g_grid_h;
            cu = p_cuMemcpy2DAsync(&d, g_stream);
            if (cu == CUDA_SUCCESS)
                cu = p_cuStreamSynchronize(g_stream);
            if (cu == CUDA_SUCCESS)
            {
                g_executes++;
                if (g_executes == 1)
                {
                    char buf[256] = {};
                    snprintf(buf, sizeof(buf), "[dali11] first execute OK: grid %ux%u outPitch %u",
                             g_grid_w, g_grid_h, g_out_pitch);
                    dbglog(buf);
                }
                have_vectors = true;
            }
            else if (g_exec_ok)
            {
                g_exec_ok = false;
                set_cu_status_locked("output download failed", cu);
            }
        }
    }
    if (p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(nullptr);

    if (cu != CUDA_SUCCESS)
    {
        g_last_cu_code = static_cast<int>(cu);
        set_status_locked(cuda_error_text(cu).c_str(), reshade::log::level::warning);
        return;
    }
    g_scene_active = (g_last_checksum != 0 && sum != g_last_checksum);
    g_last_checksum = sum;
    g_frames_captured++;
    g_memcpys_done++;

    // ---- CPU: int16 -> half into upload staging + stats; 2nd list ----
    if (have_vectors && g_upload_ptr != nullptr && g_out_tex.handle != 0 && g_out_srv.handle != 0)
    {
        const size_t total = static_cast<size_t>(g_grid_w) * g_grid_h;
        double acc = 0.0;
        size_t moving = 0;
        for (size_t i = 0; i < total; ++i)
        {
            // S10.5 hardware units -> px (g_flow_scale, default 1/32).
            const float fx = static_cast<float>(g_of_cpu[i * 2]) * g_flow_scale;
            const float fy = static_cast<float>(g_of_cpu[i * 2 + 1]) * g_flow_scale;
            const size_t row = i / g_grid_w, col = i % g_grid_w;
            uint16_t *px = reinterpret_cast<uint16_t *>(static_cast<uint8_t *>(g_upload_ptr) +
                                                        row * g_up_pitch + col * 4);
            px[0] = float_to_half(fx);
            px[1] = float_to_half(fy);
            const double len = sqrt(static_cast<double>(fx) * fx + static_cast<double>(fy) * fy);
            acc += len;
            if (len > 0.5)
                ++moving;
        }
        g_flow_avg = static_cast<float>(acc / (total != 0 ? total : 1));
        g_flow_moving_pct = 100.0f * static_cast<float>(moving) / static_cast<float>(total != 0 ? total : 1);

        if (SUCCEEDED(g_cmd_alloc->Reset()) && SUCCEEDED(g_cmd_list->Reset(g_cmd_alloc, nullptr)))
        {
            if (g_out_ready)
            {
                D3D12_RESOURCE_BARRIER to_copy2 = {};
                to_copy2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_copy2.Transition.pResource = reinterpret_cast<ID3D12Resource *>(g_out_tex.handle);
                to_copy2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_copy2.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                to_copy2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                g_cmd_list->ResourceBarrier(1, &to_copy2);
            }
            {
                D3D12_TEXTURE_COPY_LOCATION up_dst = {};
                up_dst.pResource = reinterpret_cast<ID3D12Resource *>(g_out_tex.handle);
                up_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                up_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION up_src = {};
                up_src.pResource = g_upload;
                up_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                up_src.PlacedFootprint = g_up_footprint;
                g_cmd_list->CopyTextureRegion(&up_dst, 0, 0, 0, &up_src, nullptr);
            }
            // Optional injection into a named effect texture (PPOM-style compat).
            bool found = false, injected = false;
            if (g_target_tex[0] != '\0')
            {
                const effect_texture_variable var = runtime->find_texture_variable(nullptr, g_target_tex);
                if (var.handle != 0)
                {
                    found = true;
                    resource_view srv = { 0 }, srv_srgb = { 0 };
                    runtime->get_texture_binding(var, &srv, &srv_srgb);
                    if (srv.handle != 0)
                    {
                        const resource tres = dev->get_resource_from_view(srv);
                        const resource_desc tdesc = dev->get_resource_desc(tres);
                        if (tdesc.type == resource_type::texture_2d &&
                            tdesc.texture.format == format::r16g16_float &&
                            tdesc.texture.width == g_grid_w && tdesc.texture.height == g_grid_h)
                        {
                            ID3D12Resource *t = reinterpret_cast<ID3D12Resource *>(tres.handle);
                            D3D12_RESOURCE_BARRIER to_copy3 = {};
                            to_copy3.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            to_copy3.Transition.pResource = t;
                            to_copy3.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            to_copy3.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                            to_copy3.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                            g_cmd_list->ResourceBarrier(1, &to_copy3);
                            D3D12_TEXTURE_COPY_LOCATION tg_dst = {};
                            tg_dst.pResource = t;
                            tg_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            tg_dst.SubresourceIndex = 0;
                            D3D12_TEXTURE_COPY_LOCATION tg_src = {};
                            tg_src.pResource = g_upload;
                            tg_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            tg_src.PlacedFootprint = g_up_footprint;
                            g_cmd_list->CopyTextureRegion(&tg_dst, 0, 0, 0, &tg_src, nullptr);
                            D3D12_RESOURCE_BARRIER to_sr = {};
                            to_sr.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            to_sr.Transition.pResource = t;
                            to_sr.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            to_sr.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                            to_sr.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                            g_cmd_list->ResourceBarrier(1, &to_sr);
                            injected = true;
                        }
                    }
                }
            }
            g_target_found = found;
            g_target_ok = injected;
            {
                D3D12_RESOURCE_BARRIER to_sr2 = {};
                to_sr2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_sr2.Transition.pResource = reinterpret_cast<ID3D12Resource *>(g_out_tex.handle);
                to_sr2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_sr2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                to_sr2.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                g_cmd_list->ResourceBarrier(1, &to_sr2);
            }
            g_out_ready = true;
            if (SUCCEEDED(g_cmd_list->Close()))
            {
                ID3D12CommandList *lists2[] = { g_cmd_list };
                native_queue->ExecuteCommandLists(1, lists2);
                runtime->update_texture_bindings("MotionVectors", g_out_srv, g_out_srv);
            }
        }
    }
}

static void draw_overlay(effect_runtime *)
{
    std::string status;
    bool d3d12_seen = false;
    bool session = false;
    bool caps_ok = false;
    uint32_t wmin = 0, wmax = 0, hmin = 0, hmax = 0, grids = 0;
    uint64_t frames = 0, memcpys = 0, skips = 0, executes = 0;
    uint32_t fw = 0, fh = 0;
    bool active = false;
    bool target_found = false, target_ok = false;
    float flow_avg = 0.0f, flow_moving = 0.0f, flow_scale = 0.0f;
    char target_tex[128] = {};
    int last_cu = 0, last_of = 0;
    std::string last_of_err;
    uint32_t grids_list[8] = {};
    uint32_t grids_count = 0;
    uint32_t cap_roi = 0;
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
        frames = g_frames_captured;
        memcpys = g_memcpys_done;
        fw = g_frame_w;
        fh = g_frame_h;
        skips = g_skips;
        executes = g_executes;
        active = g_scene_active;
        target_found = g_target_found;
        target_ok = g_target_ok;
        flow_avg = g_flow_avg;
        flow_moving = g_flow_moving_pct;
        flow_scale = g_flow_scale;
        last_cu = g_last_cu_code;
        last_of = g_last_of_code;
        last_of_err = g_last_of_error;
        grids_count = (g_cap_grids_count <= 8) ? g_cap_grids_count : 8;
        for (uint32_t i = 0; i < grids_count; ++i)
            grids_list[i] = g_cap_grids_list[i];
        cap_roi = g_cap_roi;
#if defined(_MSC_VER)
        strncpy_s(target_tex, g_target_tex, sizeof(target_tex) - 1);
#else
        strncpy(target_tex, g_target_tex, sizeof(target_tex) - 1);
#endif
    }

    ImGui::TextUnformatted("NVOF Motion (phase 2b: vectors) [dali11]");

    bool enabled = g_enabled != 0;
    if (ImGui::Checkbox("Enable", &enabled))
    {
        g_enabled = enabled ? 1u : 0u;
        reshade::set_config_value(nullptr, "NVOF", "Enable", g_enabled);
        if (!enabled)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            teardown_all_locked();
            g_status.assign("disabled by user");
        }
    }

    ImGui::Text("D3D12 device seen: %s", d3d12_seen ? "yes" : "no");
    ImGui::Text("NvOF session: %s", session ? "active" : "inactive");
    if (caps_ok)
    {
        ImGui::Text("Caps: %ux%u .. %ux%u, out grids mask 0x%X", wmin, hmin, wmax, hmax, grids);
        if (grids_count > 0)
        {
            std::string gs = "Grids:";
            for (uint32_t i = 0; i < grids_count; ++i)
                gs += " " + std::to_string(grids_list[i]);
            gs += " | ROI support: " + std::to_string(cap_roi);
            ImGui::TextUnformatted(gs.c_str());
        }
    }
    if (fw != 0)
        ImGui::Text("Capture: %ux%u, frames %llu, memcpys %llu, skips %llu, executes %llu", fw, fh, frames,
                    memcpys, skips, executes);
    ImGui::Text("Scene activity: %s", active ? "yes (pixels change)" : "no");
    ImGui::Text("Flow: avg %.2f px, moving %.1f%%", flow_avg, flow_moving);
    if (ImGui::DragFloat("Flow scale (px per unit)", &flow_scale, 0.001f, 0.001f, 1.0f))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_flow_scale = flow_scale;
        reshade::set_config_value(nullptr, "NVOF", "FlowScale", g_flow_scale);
    }
    if (ImGui::InputText("Target texture (empty = off)", target_tex, sizeof(target_tex)))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if defined(_MSC_VER)
        strncpy_s(g_target_tex, target_tex, sizeof(g_target_tex) - 1);
#else
        strncpy(g_target_tex, target_tex, sizeof(g_target_tex) - 1);
#endif
        g_target_tex[sizeof(g_target_tex) - 1] = '\0';
        reshade::set_config_value(nullptr, "NVOF", "TargetTexture", g_target_tex);
    }
    if (target_tex[0] != '\0')
        ImGui::Text("Target '%s': %s", target_tex, target_ok ? "injected" : (target_found ? "found" : "not found"));
    ImGui::Text("Last codes: CU %d, OF %d", last_cu, last_of);
    if (!last_of_err.empty())
        ImGui::TextWrapped("OF detail: %s", last_of_err.c_str());
    ImGui::TextWrapped("%s", status.c_str());
}

static void register_addon_nvof()
{
    reshade::register_overlay(nullptr, draw_overlay);
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
}

static void unregister_addon_nvof()
{
    reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
}

extern "C" __declspec(dllexport) const char *NAME = "NVOF Motion";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "HW optical-flow motion vectors via NVIDIA NVOFA (phase 2b: vectors).";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_hmod = hModule;
        dbglog_init();
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::get_config_value(nullptr, "NVOF", "Enable", g_enabled);
        {
            float flow_scale = g_flow_scale;
            reshade::get_config_value(nullptr, "NVOF", "FlowScale", flow_scale);
            if (flow_scale > 0.0f)
                g_flow_scale = flow_scale;
        }
        {
            char tgt[sizeof(g_target_tex)] = {};
            size_t tgt_len = sizeof(tgt);
            if (reshade::get_config_value(nullptr, "NVOF", "TargetTexture", tgt, &tgt_len) && tgt[0] != '\0')
            {
#if defined(_MSC_VER)
                strncpy_s(g_target_tex, tgt, sizeof(g_target_tex) - 1);
#else
                strncpy(g_target_tex, tgt, sizeof(g_target_tex) - 1);
#endif
                g_target_tex[sizeof(g_target_tex) - 1] = '\0';
            }
        }
        register_addon_nvof();
        break;
    case DLL_PROCESS_DETACH:
        unregister_addon_nvof();
        reshade::unregister_addon(hModule);
        // NOTE: loaded DLLs, CUDA context and native objects are intentionally
        // NOT torn down here; driver calls inside DllMain risk loader-lock
        // deadlocks. The OS reclaims everything on process exit.
        break;
    }

    return TRUE;
}
