// NVOF Motion - phase 2a: frame capture via CPU readback (no execute yet).
//
// Per frame (reshade_begin_effects, game thread), with OUR OWN native D3D12
// command list (never touches ReShade's lists, no addon-event recursion):
//  - backbuffer (pre-effects image, 1 frame old by design; TAA/history pairing)
//    is copied into a CPU-visible readback buffer, signaled through our own
//    fence; completion is polled with zero timeout (never stalls - a busy
//    frame is simply skipped and self-heals next frame), then the pixels are
//    uploaded into the NvOF input CUDA array (HtoD) and checksummed (liveness
//    proof in overlay).
//  - NvOF session is built lazily at the real frame size on first frame and
//    rebuilt on resize (drain via wait_idle first).
// NOTE: readback roundtrip costs ~1-3 ms/frame; a zero-copy import replaces
// it once the cuImportExternalMemory (CU 201) puzzle is solved (follow-up).
//
// Assumptions (validated at runtime, reported in overlay):
//  - backbuffer is in PRESENT state at begin_effects (post-present invariant)
//  - backbuffer format is R8G8B8A8; otherwise idle with status
//  - NVOF input ABGR8 accepts our RGBA bytes (R/B swapped luma, v1 approx)
//
// Phase 2b will add: NvOFExecute, PTX int16->float convert, R16G16F output
// texture + MotionVectors binding / TargetTexture injection, preview.

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
using cuStreamCreate_fn = CUresult(CUDAAPI *)(CUstream *, unsigned int);
using cuStreamDestroy_fn = CUresult(CUDAAPI *)(CUstream);
using cuStreamSynchronize_fn = CUresult(CUDAAPI *)(CUstream);
using cuMemcpy2DAsync_fn = CUresult(CUDAAPI *)(const CUDA_MEMCPY2D *, CUstream);

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
cuStreamCreate_fn p_cuStreamCreate = nullptr;
cuStreamDestroy_fn p_cuStreamDestroy = nullptr;
cuStreamSynchronize_fn p_cuStreamSynchronize = nullptr;
cuMemcpy2DAsync_fn p_cuMemcpy2DAsync = nullptr;

NvOFCreateInstance_fn p_NvOFCreateInstanceCuda = nullptr;
NV_OF_CUDA_API_FUNCTION_LIST g_of = {};
NvOFHandle g_of_handle = nullptr;

CUcontext g_cu_ctx = nullptr;
ID3D12Device *g_owner_device = nullptr; // device owning ctx + pipeline
bool g_session_active = false;

unsigned int g_enabled = 1;
bool g_d3d12_seen = false;
std::string g_status = "not initialized yet";

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

NvOFGPUBufferHandle g_input_bufs[2] = { nullptr, nullptr };
CUarray g_input_arrays[2] = { nullptr, nullptr };

uint64_t g_frames_captured = 0;
uint64_t g_memcpys_done = 0;
int g_last_cu_code = 0;
int g_last_of_code = 0;

// cached caps for the overlay (no CUDA calls from overlay thread)
uint32_t g_cap_wmin = 0, g_cap_wmax = 0, g_cap_hmin = 0, g_cap_hmax = 0;
uint32_t g_cap_grids = 0;
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
    NVOF_RESOLVE(p_cuCtxSetCurrent, cuCtxSetCurrent_fn, "cuCtxSetCurrent", nullptr);
    NVOF_RESOLVE(p_cuGetErrorString, cuGetErrorString_fn, "cuGetErrorString", nullptr);
    NVOF_RESOLVE(p_cuStreamCreate, cuStreamCreate_fn, "cuStreamCreate", nullptr);
    NVOF_RESOLVE(p_cuStreamDestroy, cuStreamDestroy_fn, "cuStreamDestroy", nullptr);
    NVOF_RESOLVE(p_cuStreamSynchronize, cuStreamSynchronize_fn, "cuStreamSynchronize", nullptr);
    NVOF_RESOLVE(p_cuMemcpy2DAsync, cuMemcpy2DAsync_fn, "cuMemcpy2DAsync", "cuMemcpy2DAsync_v2");
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
        g_input_arrays[i] = nullptr;
    }
    if (g_of_handle != nullptr && g_of.nvOFDestroy != nullptr)
    {
        g_of.nvOFDestroy(g_of_handle);
        g_of_handle = nullptr;
    }
    g_session_active = false;
    g_caps_ok = false;
}

// Drops readback buffer + session/inputs. Keeps ctx, table, DLLs and
// size-independent objects (list/alloc/fence/event/stream). Manages ctx itself.
void teardown_pipeline_locked()
{
    if (g_cu_ctx != nullptr && p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(g_cu_ctx);

    destroy_of_locked();

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
    g_frame_w = g_frame_h = 0;

    if (p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(nullptr);
}

// Full teardown incl. CUDA context. Safe to call with partial state.
void teardown_all_locked()
{
    if (g_cu_ctx != nullptr && p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(g_cu_ctx);

    teardown_pipeline_locked();

    // teardown_pipeline_locked detached; re-attach for the work below.
    if (g_cu_ctx != nullptr && p_cuCtxSetCurrent != nullptr)
        p_cuCtxSetCurrent(g_cu_ctx);

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
        if (p_cuCtxDestroy != nullptr)
            p_cuCtxDestroy(g_cu_ctx);
        g_cu_ctx = nullptr;
    }
    g_owner_device = nullptr;
}

// (Re)builds NvOF session + 2 input buffers at W x H. ctx must be current.
bool create_of_locked(uint32_t w, uint32_t h)
{
    destroy_of_locked();

    NvOFHandle hOf = nullptr;
    NV_OF_STATUS st = g_of.nvCreateOpticalFlowCuda(g_cu_ctx, &hOf);
    if (st != NV_OF_SUCCESS || hOf == nullptr)
    {
        g_last_of_code = static_cast<int>(st);
        set_status_locked("nvCreateOpticalFlowCuda failed", reshade::log::level::warning);
        return false;
    }
    g_of_handle = hOf;

    NV_OF_INIT_PARAMS init = {};
    init.width = w;
    init.height = h;
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
        g_last_of_code = static_cast<int>(st);
        set_status_locked("nvOFInit failed", reshade::log::level::warning);
        destroy_of_locked();
        return false;
    }

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

    for (int i = 0; i < 2; ++i)
    {
        NV_OF_BUFFER_DESCRIPTOR desc = {};
        desc.width = w;
        desc.height = h;
        desc.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
        desc.bufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
        st = g_of.nvOFCreateGPUBufferCuda(hOf, &desc, NV_OF_CUDA_BUFFER_TYPE_CUARRAY, &g_input_bufs[i]);
        if (st != NV_OF_SUCCESS || g_input_bufs[i] == nullptr)
        {
            g_last_of_code = static_cast<int>(st);
            set_status_locked("nvOFCreateGPUBufferCuda (input) failed", reshade::log::level::warning);
            destroy_of_locked();
            return false;
        }
        g_input_arrays[i] = g_of.nvOFGPUBufferGetCUarray(g_input_bufs[i]);
        if (g_input_arrays[i] == nullptr)
        {
            set_status_locked("nvOFGPUBufferGetCUarray failed", reshade::log::level::warning);
            destroy_of_locked();
            return false;
        }
    }

    {
        const std::string msg =
            "[NVOF Motion] session init ok (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        reshade::log::message(reshade::log::level::info, msg.c_str());
    }

    g_session_active = true;
    return true;
}

// (Re)builds the whole frame pipeline at W x H (readback + session).
// Caller must have drained the GPU first (wait_idle) when rebuilding.
bool build_pipeline_locked(ID3D12Device *d3d12, uint32_t w, uint32_t h)
{
    if (p_cuCtxSetCurrent == nullptr || g_cu_ctx == nullptr)
        return false;
    p_cuCtxSetCurrent(g_cu_ctx);

    // On any failure below, the caller runs teardown_pipeline_locked(),
    // which safely drops partial state.
    // NvOF session + input dims are aligned up (HW grid friendliness);
    // readback + memcpy stay at the real frame size (subregion copy).
    const uint32_t sw = (w + 7u) & ~7u;
    const uint32_t sh = (h + 7u) & ~7u;
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
        if (!create_of_locked(sw, sh))
            break;

        g_frame_w = w;
        g_frame_h = h;
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

    CUcontext ctx = nullptr;
    cu = p_cuCtxCreate(&ctx, 0 /* CU_CTX_SCHED_AUTO */, static_cast<CUdevice>(matched));
    if (cu != CUDA_SUCCESS || ctx == nullptr)
    {
        set_status_locked(cuda_error_text(cu).c_str(), reshade::log::level::warning);
        return;
    }
    g_cu_ctx = ctx;

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
            g_of.nvOFDestroyGPUBufferCuda == nullptr)
        {
            g_last_of_code = static_cast<int>(st);
            set_status_locked("NvOFAPICreateInstanceCuda failed or incomplete", reshade::log::level::warning);
            p_cuCtxSetCurrent(nullptr);
            return;
        }
    }

    g_owner_device = d3d12;
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
            teardown_pipeline_locked();
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
        teardown_pipeline_locked();
        const bool built = build_pipeline_locked(d3d12, w, h);
        if (!built)
        {
            teardown_pipeline_locked(); // drop partial state, retry next frame
            return;                     // status already set by the failing step
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
        p_cuCtxSetCurrent(g_cu_ctx);
    CUDA_MEMCPY2D m = {};
    m.srcXInBytes = 0;
    m.srcY = 0;
    m.srcMemoryType = CU_MEMORYTYPE_HOST;
    m.srcHost = g_readback_ptr;
    m.srcPitch = g_readback_pitch;
    m.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    m.dstArray = g_input_arrays[g_frames_captured % 2];
    m.WidthInBytes = static_cast<size_t>(w) * 4;
    m.Height = h;
    CUresult cu = p_cuMemcpy2DAsync(&m, g_stream);
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
}

static void draw_overlay(effect_runtime *)
{
    std::string status;
    bool d3d12_seen = false;
    bool session = false;
    bool caps_ok = false;
    uint32_t wmin = 0, wmax = 0, hmin = 0, hmax = 0, grids = 0;
    uint64_t frames = 0, memcpys = 0, skips = 0;
    uint32_t fw = 0, fh = 0;
    bool active = false;
    int last_cu = 0, last_of = 0;
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
        active = g_scene_active;
        last_cu = g_last_cu_code;
        last_of = g_last_of_code;
    }

    ImGui::TextUnformatted("NVOF Motion (phase 2a: capture, no execute)");

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
        ImGui::Text("Caps: %ux%u .. %ux%u, out grids mask 0x%X", wmin, hmin, wmax, hmax, grids);
    if (fw != 0)
        ImGui::Text("Capture: %ux%u, frames %llu, memcpys %llu, skips %llu", fw, fh, frames, memcpys, skips);
    ImGui::Text("Scene activity: %s", active ? "yes (pixels change)" : "no");
    ImGui::Text("Last codes: CU %d, OF %d", last_cu, last_of);
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
    "HW optical-flow motion vectors via NVIDIA NVOFA (phase 2a: capture, no execute).";

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
        // NOTE: loaded DLLs, CUDA context and native objects are intentionally
        // NOT torn down here; driver calls inside DllMain risk loader-lock
        // deadlocks. The OS reclaims everything on process exit.
        break;
    }

    return TRUE;
}
