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

#include "nvOpticalFlowCommon.h" // mbucchia/Optical-Flow-SDK NvOFInterface (API 5.0)
#include "nvOpticalFlowD3D12.h"

using namespace reshade;
using namespace reshade::api;

// ---- explicit NvOF D3D12 imports (nvofapi64.dll, no import lib needed) ----
using NvOFCreateInstanceD3D12_fn = NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST *);

namespace {

std::mutex g_mutex;

HMODULE g_nvof_dll = nullptr;
NvOFCreateInstanceD3D12_fn p_NvOFCreateInstanceD3D12 = nullptr;
NV_OF_D3D12_API_FUNCTION_LIST g_of12 = {};
NvOFHandle g_of_handle = nullptr;

// Production grid (phase 2b target: grid-4 vectors for interpolation).
constexpr uint32_t kOutGrid = 4;
ID3D12Device *g_owner_device = nullptr; // game device owning the NvOF session
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

// ---- frame pipeline (all guarded by g_mutex) ----
ID3D12CommandAllocator *g_cmd_alloc = nullptr;
ID3D12GraphicsCommandList *g_cmd_list = nullptr;
ID3D12Fence *g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_frame_event = nullptr; // signaled when the copy of a frame is done

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

// ---- NvOF D3D12 registered resources (no CUDA) ----
NvOFGPUBufferHandle g_in_bufs[2] = { nullptr, nullptr };
ID3D12Resource *g_in_res[2] = { nullptr, nullptr }; // DEFAULT R8_UNORM WxH
ID3D12Resource *g_in_up[2] = { nullptr, nullptr }; // UPLOAD staging (byte buffers)
void *g_in_up_ptr[2] = { nullptr, nullptr };       // persistent maps
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_in_foot[2] = {}; // footprint of R8 inputs
std::vector<uint8_t> g_gray; // RGBA->luma staging (session-pitched rows)
uint32_t g_gray_pitch = 0;

// ---- phase 2b: execute output + conversion + exposure ----
NvOFGPUBufferHandle g_out_buf = nullptr;
ID3D12Resource *g_out_res = nullptr; // DEFAULT R16G16_SINT grid
ID3D12Resource *g_out_down = nullptr; // READBACK staging
void *g_out_down_ptr = nullptr;      // persistent map
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_out_foot = {};
ID3D12Fence *g_app_fence = nullptr; // upload-complete (app side)
uint64_t g_app_val = 0;
ID3D12Fence *g_ofa_fence = nullptr; // exec-complete (NVOFA side)
uint64_t g_ofa_val = 0;
std::vector<int16_t> g_of_cpu; // int16 pairs staging for convert
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
int g_last_of_code = 0;
bool g_synth_test = false; // overlay toggle: synthetic +8px pair instead of game frames
bool g_show_tex = false; // overlay toggle: debug view of the flow texture (PPOM-style)
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

// Blocking CPU wait on a fence value (stock AppOFD3D12 pattern).
void wait_fence_value(ID3D12Fence *fence, uint64_t value)
{
    if (fence == nullptr || fence->GetCompletedValue() >= value)
        return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr)
        return;
    if (SUCCEEDED(fence->SetEventOnCompletion(value, ev)))
        WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
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

// Loads nvofapi64.dll + resolves the D3D12 entry point (no CUDA involved).
bool resolve_nvof_locked()
{
    if (g_nvof_dll == nullptr)
    {
        g_nvof_dll = LoadLibraryW(L"nvofapi64.dll");
        if (g_nvof_dll == nullptr)
        {
            set_status_locked("nvofapi64.dll NOT found - no NVIDIA driver, staying idle",
                              reshade::log::level::warning);
            return false;
        }
    }
    if (p_NvOFCreateInstanceD3D12 == nullptr)
    {
        p_NvOFCreateInstanceD3D12 = reinterpret_cast<NvOFCreateInstanceD3D12_fn>(
            GetProcAddress(g_nvof_dll, "NvOFAPICreateInstanceD3D12"));
        if (p_NvOFCreateInstanceD3D12 == nullptr)
        {
            set_status_locked("nvofapi64.dll present but NvOFAPICreateInstanceD3D12 missing",
                              reshade::log::level::warning);
            return false;
        }
    }
    return true;
}

// Destroys NvOF session (unregister first!) + D3D12 resources + NvOF fences.
void destroy_of_locked()
{
    for (int i = 0; i < 2; ++i)
    {
        if (g_in_bufs[i] != nullptr && g_of12.nvOFUnregisterResourceD3D12 != nullptr)
        {
            NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 up = {};
            up.hOFGpuBuffer = g_in_bufs[i];
            g_of12.nvOFUnregisterResourceD3D12(&up);
        }
        g_in_bufs[i] = nullptr;
        if (g_in_res[i] != nullptr)
        {
            g_in_res[i]->Release();
            g_in_res[i] = nullptr;
        }
        if (g_in_up[i] != nullptr)
        {
            g_in_up[i]->Unmap(0, nullptr);
            g_in_up[i]->Release();
            g_in_up[i] = nullptr;
        }
        g_in_up_ptr[i] = nullptr;
    }
    if (g_out_buf != nullptr && g_of12.nvOFUnregisterResourceD3D12 != nullptr)
    {
        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 up = {};
        up.hOFGpuBuffer = g_out_buf;
        g_of12.nvOFUnregisterResourceD3D12(&up);
    }
    g_out_buf = nullptr;
    if (g_out_res != nullptr)
    {
        g_out_res->Release();
        g_out_res = nullptr;
    }
    if (g_out_down != nullptr)
    {
        g_out_down->Unmap(0, nullptr);
        g_out_down->Release();
        g_out_down = nullptr;
    }
    g_out_down_ptr = nullptr;
    if (g_app_fence != nullptr)
    {
        g_app_fence->Release();
        g_app_fence = nullptr;
    }
    g_app_val = 0;
    if (g_ofa_fence != nullptr)
    {
        g_ofa_fence->Release();
        g_ofa_fence = nullptr;
    }
    g_ofa_val = 0;
    if (g_of_handle != nullptr && g_of12.nvOFDestroy != nullptr)
    {
        g_of12.nvOFDestroy(g_of_handle);
        g_of_handle = nullptr;
    }
    g_session_active = false;
    g_caps_ok = false;
}

// Drops readback buffer + session/inputs (+ linear scratch).
// Output texture/SRV are destroyed only when gpu_idle (GPU may sample them).
void teardown_pipeline_locked(bool gpu_idle)
{
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
}

// Full teardown incl. NvOF fences/DLL state. Safe to call with partial state.
void teardown_all_locked()
{
    teardown_pipeline_locked(false); // no drain here: leak cb texture

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

    g_owner_device = nullptr;
    g_api_dev = nullptr;
}

// POD-only NvOF D3D12 bring-up with SEH guard: any driver crash inside becomes
// a reported status (stage + exception code) instead of a dead process.
// Stages: 1 = create handle, 2 = init (+ formats), 3 = fences,
// 4/5 = input resources, 6 = output resource.
// Returns false on error or exception. On exception, handles are deliberately
// leaked (never touch the driver again on that path); g_crashed_nvof is set.
// NOTE: no unwindable locals in here (C2712) - POD and globals only.
bool create_of_nvof_locked(ID3D12Device *d3d12, uint32_t w, uint32_t h)
{
    g_crash_stage = 0;
    g_crashed_nvof = false;
    __try
    {
        g_crash_stage = 1;
        NvOFHandle hOf = nullptr;
        NV_OF_STATUS st = g_of12.nvCreateOpticalFlowD3D12(d3d12, &hOf);
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
        init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = NV_OF_PERF_LEVEL_FAST; // production preset
        init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        init.enableExternalHints = NV_OF_FALSE;
        init.enableOutputCost = NV_OF_FALSE;
        init.hPrivData = nullptr;

        st = g_of12.nvOFInit(hOf, &init);
        if (st != NV_OF_SUCCESS)
        {
            g_last_of_code = static_cast<int>(st);
            return false;
        }

        // Surface formats must include our R8 (gray in) + R16G16_SINT (flow out).
        {
            uint32_t n = 0;
            if (g_of12.nvOFGetSurfaceFormatCountD3D12(hOf, NV_OF_BUFFER_USAGE_INPUT,
                                                      NV_OF_MODE_OPTICALFLOW, &n) != NV_OF_SUCCESS ||
                n == 0 || n > 16)
            {
                g_last_of_code = -1;
                return false;
            }
            DXGI_FORMAT fmts[16] = {};
            if (g_of12.nvOFGetSurfaceFormatD3D12(hOf, NV_OF_BUFFER_USAGE_INPUT, NV_OF_MODE_OPTICALFLOW,
                                                fmts) != NV_OF_SUCCESS)
            {
                g_last_of_code = -2;
                return false;
            }
            bool have_r8 = false;
            for (uint32_t i = 0; i < n; ++i)
                if (fmts[i] == DXGI_FORMAT_R8_UNORM)
                    have_r8 = true;
            if (!have_r8)
            {
                g_last_of_code = -3;
                return false;
            }
        }
        {
            uint32_t n = 0;
            if (g_of12.nvOFGetSurfaceFormatCountD3D12(hOf, NV_OF_BUFFER_USAGE_OUTPUT,
                                                      NV_OF_MODE_OPTICALFLOW, &n) != NV_OF_SUCCESS ||
                n == 0 || n > 16)
            {
                g_last_of_code = -4;
                return false;
            }
            DXGI_FORMAT fmts[16] = {};
            if (g_of12.nvOFGetSurfaceFormatD3D12(hOf, NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_MODE_OPTICALFLOW,
                                                fmts) != NV_OF_SUCCESS)
            {
                g_last_of_code = -5;
                return false;
            }
            bool have_flow = false;
            for (uint32_t i = 0; i < n; ++i)
                if (fmts[i] == DXGI_FORMAT_R16G16_SINT)
                    have_flow = true;
            if (!have_flow)
            {
                g_last_of_code = -6;
                return false;
            }
        }

        g_crash_stage = 3;
        if (FAILED(d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_app_fence))) ||
            g_app_fence == nullptr ||
            FAILED(d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_ofa_fence))) ||
            g_ofa_fence == nullptr)
        {
            g_last_of_code = -7;
            return false;
        }
        g_app_val = 0;
        g_ofa_val = 0;

        for (int i = 0; i < 2; ++i)
        {
            g_crash_stage = 4 + i;
            D3D12_RESOURCE_DESC td = {};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w;
            td.Height = h;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            if (FAILED(d3d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&g_in_res[i]))) ||
                g_in_res[i] == nullptr)
            {
                g_last_of_code = -10 - i;
                return false;
            }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            UINT64 total = 0;
            d3d12->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
            if (total == 0 || fp.Footprint.RowPitch == 0)
            {
                g_last_of_code = -20 - i;
                return false;
            }
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES uhp = {};
            uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
            uhp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            uhp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            void *mapped = nullptr;
            if (FAILED(d3d12->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&g_in_up[i]))) ||
                g_in_up[i] == nullptr || FAILED(g_in_up[i]->Map(0, nullptr, &mapped)) || mapped == nullptr)
            {
                g_last_of_code = -30 - i;
                return false;
            }
            g_in_up_ptr[i] = mapped;
            g_in_foot[i] = fp;
            NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp = {};
            rp.resource = g_in_res[i];
            rp.inputFencePoint.fence = g_app_fence;
            rp.inputFencePoint.value = 0;
            rp.hOFGpuBuffer = &g_in_bufs[i];
            rp.outputFencePoint.fence = g_ofa_fence;
            rp.outputFencePoint.value = 0;
            st = g_of12.nvOFRegisterResourceD3D12(hOf, &rp);
            if (st != NV_OF_SUCCESS || g_in_bufs[i] == nullptr)
            {
                g_last_of_code = static_cast<int>(st);
                return false;
            }
        }
        g_crash_stage = 6;
        {
            const uint32_t gw = (w + kOutGrid - 1) / kOutGrid;
            const uint32_t gh = (h + kOutGrid - 1) / kOutGrid;
            D3D12_RESOURCE_DESC td = {};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = gw;
            td.Height = gh;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R16G16_SINT;
            td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            if (FAILED(d3d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&g_out_res))) ||
                g_out_res == nullptr)
            {
                g_last_of_code = -40;
                return false;
            }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            UINT64 total = 0;
            d3d12->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
            if (total == 0 || fp.Footprint.RowPitch == 0)
            {
                g_last_of_code = -41;
                return false;
            }
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES rhp = {};
            rhp.Type = D3D12_HEAP_TYPE_READBACK;
            rhp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            rhp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            void *mapped = nullptr;
            if (FAILED(d3d12->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&g_out_down))) ||
                g_out_down == nullptr || FAILED(g_out_down->Map(0, nullptr, &mapped)) || mapped == nullptr)
            {
                g_last_of_code = -42;
                return false;
            }
            g_out_down_ptr = mapped;
            g_out_foot = fp;
            NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp = {};
            rp.resource = g_out_res;
            rp.inputFencePoint.fence = g_app_fence;
            rp.inputFencePoint.value = 0;
            rp.hOFGpuBuffer = &g_out_buf;
            rp.outputFencePoint.fence = g_ofa_fence;
            rp.outputFencePoint.value = 0;
            st = g_of12.nvOFRegisterResourceD3D12(hOf, &rp);
            if (st != NV_OF_SUCCESS || g_out_buf == nullptr)
            {
                g_last_of_code = static_cast<int>(st);
                return false;
            }
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

// (Re)builds NvOF D3D12 session + registered resources at W x H.
bool create_of_locked(ID3D12Device *d3d12, uint32_t w, uint32_t h)
{
    destroy_of_locked();

    if (!create_of_nvof_locked(d3d12, w, h))
    {
        if (g_crashed_nvof)
        {
            // Driver crashed mid-bring-up: leak handles on purpose, report it.
            g_of_handle = nullptr;
            g_in_bufs[0] = nullptr;
            g_in_bufs[1] = nullptr;
            g_in_res[0] = nullptr;
            g_in_res[1] = nullptr;
            g_in_up[0] = nullptr;
            g_in_up[1] = nullptr;
            g_in_up_ptr[0] = nullptr;
            g_in_up_ptr[1] = nullptr;
            g_out_buf = nullptr;
            g_out_res = nullptr;
            g_out_down = nullptr;
            g_out_down_ptr = nullptr;
            g_app_fence = nullptr;
            g_ofa_fence = nullptr;
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
        if (g_of12.nvOFGetCaps(g_of_handle, cap, &val, &size) == NV_OF_SUCCESS)
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
    // Two-stage caps: real supported-grids array + ROI support.
    if (g_of12.nvOFGetCaps != nullptr)
    {
        uint32_t n = 0;
        if (g_of12.nvOFGetCaps(g_of_handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &n) ==
                NV_OF_SUCCESS &&
            n > 0 && n <= 8)
        {
            uint32_t vals[8] = {};
            uint32_t m = n;
            if (g_of12.nvOFGetCaps(g_of_handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, vals, &m) ==
                NV_OF_SUCCESS)
            {
                g_cap_grids_count = (m <= 8) ? m : 8;
                for (uint32_t i = 0; i < g_cap_grids_count; ++i)
                    g_cap_grids_list[i] = vals[i];
            }
        }
        uint32_t roi = 0, rsz = sizeof(roi);
        if (g_of12.nvOFGetCaps(g_of_handle, NV_OF_CAPS_SUPPORT_ROI, &roi, &rsz) == NV_OF_SUCCESS)
            g_cap_roi = roi;
    }

    // int16 pairs staging for the device->host download.
    g_grid_w = (w + kOutGrid - 1) / kOutGrid;
    g_grid_h = (h + kOutGrid - 1) / kOutGrid;
    g_of_cpu.assign(static_cast<size_t>(g_grid_w) * g_grid_h * 2, 0);
    g_gray.assign(static_cast<size_t>(w) * h, 0); // luma staging, session-pitched
    g_gray_pitch = w;

    {
        const std::string msg =
            "[NVOF Motion] D3D12 session init ok (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        reshade::log::message(reshade::log::level::info, msg.c_str());
        char buf[768] = {};
        snprintf(buf, sizeof(buf),
                 "[d3d12-10] session %ux%u api=0x%X init(grid4,hintUndef,noExt,noCost,FAST,OF,inR8,tempON) caps %u-%u x %u-%u "
                 "grids[%u]{%u %u %u %u %u %u %u %u} roi %u | "
                 "in0=%p in1=%p | out=%p outgrid=%ux%u",
                 w, h, static_cast<unsigned int>(NV_OF_API_VERSION), g_cap_wmin, g_cap_wmax, g_cap_hmin,
                 g_cap_hmax, g_cap_grids_count,
                 g_cap_grids_list[0], g_cap_grids_list[1], g_cap_grids_list[2], g_cap_grids_list[3],
                 g_cap_grids_list[4], g_cap_grids_list[5], g_cap_grids_list[6], g_cap_grids_list[7],
                 g_cap_roi, g_in_res[0], g_in_res[1], g_out_res, g_grid_w, g_grid_h);
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
    // On any failure below, the caller runs teardown_pipeline_locked(),
    // which safely drops partial state.
    // Exact session dims (probe verdict: padding breaks execute).
    const uint32_t sw = w;
    const uint32_t sh = h;
    {
        char buf[128] = {};
        snprintf(buf, sizeof(buf), "[d3d12-10] build req %ux%u -> session %ux%u", w, h, sw, sh);
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
            const uint32_t gw = (sw + kOutGrid - 1) / kOutGrid, gh = (sh + kOutGrid - 1) / kOutGrid;
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
        if (!create_of_locked(d3d12, sw, sh))
            break;

        g_frame_w = w;
        g_frame_h = h;
        ok = true;
    } while (false);

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
    if (g_owner_device == d3d12 && g_of_handle != nullptr)
        return; // already serving this device (session comes lazily)
    teardown_all_locked(); // clean start (also migrates multi-GPU)

    if (!resolve_nvof_locked())
        return;

    g_of12 = {};
    {
        const NV_OF_STATUS st = p_NvOFCreateInstanceD3D12(NV_OF_API_VERSION, &g_of12);
        if (st != NV_OF_SUCCESS || g_of12.nvCreateOpticalFlowD3D12 == nullptr ||
            g_of12.nvOFInit == nullptr || g_of12.nvOFGetSurfaceFormatCountD3D12 == nullptr ||
            g_of12.nvOFGetSurfaceFormatD3D12 == nullptr || g_of12.nvOFRegisterResourceD3D12 == nullptr ||
            g_of12.nvOFUnregisterResourceD3D12 == nullptr || g_of12.nvOFExecuteD3D12 == nullptr ||
            g_of12.nvOFDestroy == nullptr || g_of12.nvOFGetLastError == nullptr ||
            g_of12.nvOFGetCaps == nullptr)
        {
            g_last_of_code = static_cast<int>(st);
            set_status_locked("NvOFAPICreateInstanceD3D12 failed or incomplete", reshade::log::level::warning);
            return;
        }
    }

    g_owner_device = d3d12;
    g_api_dev = device;
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
    if (g_owner_device == nullptr)
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

    // RGBA -> luma staging (unchanged); then upload into the registered R8 input.
    // Synthetic self-test (overlay toggle): shifting pattern, constant +8px flow.
    // High-contrast 8px checker (FAST-trackable; smooth gradients go blind).
    if (g_synth_test)
    {
        const int dx = static_cast<int>((8u * static_cast<uint32_t>(g_frames_captured)) % w);
        uint8_t *gd = g_gray.data();
        for (uint32_t y = 0; y < h; ++y)
        {
            uint8_t *drow = gd + static_cast<size_t>(y) * g_gray_pitch;
            const int gy = (static_cast<int>(y) * 255) / static_cast<int>(h);
            for (uint32_t x = 0; x < w; ++x)
            {
                const int sx = (static_cast<int>(x) + static_cast<int>(w) - dx) % static_cast<int>(w);
                const int gx = (sx * 255) / static_cast<int>(w);
                int t = (gx + gy) / 2 + ((((sx / 8 + static_cast<int>(y) / 8) & 1) != 0) ? 80 : -80);
                if (t < 0)
                    t = 0;
                if (t > 255)
                    t = 255;
                drow[x] = static_cast<uint8_t>(t);
            }
        }
    }
    else if (g_gray_pitch == 0 || g_gray.size() < static_cast<size_t>(g_gray_pitch) * h)
    {
        set_status_locked("session/gray mismatch, skipping frame", reshade::log::level::warning);
        return;
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
    const int fill_idx = static_cast<int>(g_frames_captured % 2);
    // Liveness checksum over the luma staging (what NVOFA actually sees).
    uint64_t sum = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    {
        const uint8_t *gd = g_gray.data();
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *row = gd + static_cast<size_t>(y) * g_gray_pitch;
            for (uint32_t x = 0; x < w; ++x)
            {
                sum ^= row[x];
                sum *= 1099511628211ULL; // FNV-1a prime
            }
        }
    }
    // Upload: gray rows -> mapped UPLOAD staging -> registered R8 input (own list).
    {
        uint8_t *dst = static_cast<uint8_t *>(g_in_up_ptr[fill_idx]);
        const uint8_t *src = g_gray.data();
        const uint32_t pitch = g_in_foot[fill_idx].Footprint.RowPitch;
        const size_t off = g_in_foot[fill_idx].Offset;
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(dst + off + static_cast<size_t>(y) * pitch, src + static_cast<size_t>(y) * w, w);
    }
    if (FAILED(g_cmd_alloc->Reset()) || FAILED(g_cmd_list->Reset(g_cmd_alloc, nullptr)))
    {
        set_status_locked("NvOF upload list reset failed", reshade::log::level::warning);
        return;
    }
    {
        D3D12_RESOURCE_BARRIER to_dst = {};
        to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_dst.Transition.pResource = g_in_res[fill_idx];
        to_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_cmd_list->ResourceBarrier(1, &to_dst);
    }
    {
        D3D12_TEXTURE_COPY_LOCATION copy_dst = {};
        copy_dst.pResource = g_in_res[fill_idx];
        copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copy_dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION copy_src = {};
        copy_src.pResource = g_in_up[fill_idx];
        copy_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        copy_src.PlacedFootprint = g_in_foot[fill_idx];
        g_cmd_list->CopyTextureRegion(&copy_dst, 0, 0, 0, &copy_src, nullptr);
    }
    {
        D3D12_RESOURCE_BARRIER to_common = {};
        to_common.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_common.Transition.pResource = g_in_res[fill_idx];
        to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_cmd_list->ResourceBarrier(1, &to_common);
    }
    if (FAILED(g_cmd_list->Close()))
    {
        set_status_locked("NvOF upload list close failed", reshade::log::level::warning);
        return;
    }
    {
        ID3D12CommandList *lists[] = { g_cmd_list };
        native_queue->ExecuteCommandLists(1, lists);
        native_queue->Signal(g_app_fence, ++g_app_val);
    }
    wait_fence_value(g_app_fence, g_app_val); // upload copy done (blocking, ~instant)

    // ---- NvOF D3D12 execute on pair (prev, cur), convert on CPU, stats ----
    // Fully synchronous frames (blocking fence waits); only when both
    // input resources hold a frame (from the 2nd captured frame on).
    bool have_vectors = false;
    if (g_frames_captured >= 1 && g_in_bufs[0] != nullptr && g_in_bufs[1] != nullptr &&
        g_out_buf != nullptr && !g_of_cpu.empty())
    {
        const int just_filled = fill_idx;
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in12 = {};
        in12.inputFrame = g_in_bufs[1 - just_filled]; // older (stock order)
        in12.referenceFrame = g_in_bufs[just_filled];
        in12.externalHints = nullptr;
        in12.disableTemporalHints = NV_OF_FALSE; // stock/guide default; ON healthy too (d3d12-3)
        NV_OF_FENCE_POINT inFp = {};
        inFp.fence = g_app_fence;
        inFp.value = g_app_val;
        in12.fencePoint = &inFp;
        in12.numFencePoints = 1; // driver requires >= 1 (single input fence)
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out12 = {};
        out12.outputBuffer = g_out_buf;
        out12.outputCostBuffer = nullptr;
        out12.hPrivData = nullptr;
        NV_OF_FENCE_POINT outFp = {};
        outFp.fence = g_ofa_fence;
        outFp.value = ++g_ofa_val;
        out12.fencePoint = &outFp;
        const NV_OF_STATUS ost = g_of12.nvOFExecuteD3D12(g_of_handle, &in12, &out12);
        if (ost != NV_OF_SUCCESS)
        {
            g_last_of_code = static_cast<int>(ost);
            // Driver detail string (once per failure burst; overlay shows it).
            if (g_of12.nvOFGetLastError != nullptr)
            {
                char err[256] = {};
                uint32_t errsz = sizeof(err);
                if (g_of12.nvOFGetLastError(g_of_handle, err, &errsz) == NV_OF_SUCCESS && err[0] != '\0')
                    g_last_of_error.assign(err);
            }
            if (g_dbg_fail_lines < 5)
            {
                g_dbg_fail_lines++;
                char buf[512] = {};
                snprintf(buf, sizeof(buf),
                         "[d3d12-10] exec FAIL #%d: OF %d detail='%s' inHandle=%p refHandle=%p outHandle=%p "
                         "frames=%llu",
                         g_dbg_fail_lines, static_cast<int>(ost), g_last_of_error.c_str(),
                         in12.inputFrame, in12.referenceFrame, out12.outputBuffer,
                         static_cast<unsigned long long>(g_frames_captured));
                dbglog(buf);
            }
            if (g_exec_ok)
            {
                g_exec_ok = false;
                std::string msg = std::string("nvOFExecuteD3D12 failed") +
                    (g_last_of_error.empty() ? "" : std::string(" : ") + g_last_of_error);
                set_status_locked(msg.c_str(), reshade::log::level::warning);
            }
        }
        else
        {
            g_exec_ok = true;
            // SHORT2 vectors -> host: queue-wait exec, copy to staging, CPU wait.
            if (FAILED(g_cmd_alloc->Reset()) || FAILED(g_cmd_list->Reset(g_cmd_alloc, nullptr)))
            {
                set_status_locked("NvOF download list reset failed", reshade::log::level::warning);
                return;
            }
            native_queue->Wait(g_ofa_fence, g_ofa_val);
            {
                D3D12_RESOURCE_BARRIER to_src = {};
                to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_src.Transition.pResource = g_out_res;
                to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                g_cmd_list->ResourceBarrier(1, &to_src);
            }
            {
                D3D12_TEXTURE_COPY_LOCATION copy_dst = {};
                copy_dst.pResource = g_out_down;
                copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                copy_dst.PlacedFootprint = g_out_foot;
                D3D12_TEXTURE_COPY_LOCATION copy_src = {};
                copy_src.pResource = g_out_res;
                copy_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                copy_src.SubresourceIndex = 0;
                g_cmd_list->CopyTextureRegion(&copy_dst, 0, 0, 0, &copy_src, nullptr);
            }
            {
                D3D12_RESOURCE_BARRIER to_common = {};
                to_common.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_common.Transition.pResource = g_out_res;
                to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                g_cmd_list->ResourceBarrier(1, &to_common);
            }
            if (FAILED(g_cmd_list->Close()))
            {
                set_status_locked("NvOF download list close failed", reshade::log::level::warning);
                return;
            }
            {
                ID3D12CommandList *lists[] = { g_cmd_list };
                native_queue->ExecuteCommandLists(1, lists);
                native_queue->Signal(g_fence, ++g_fence_value);
            }
            g_fence->SetEventOnCompletion(g_fence_value, g_frame_event);
            WaitForSingleObject(g_frame_event, INFINITE); // download done (stock pattern)
            {
                const uint8_t *src = static_cast<const uint8_t *>(g_out_down_ptr) + g_out_foot.Offset;
                const uint32_t pitch = g_out_foot.Footprint.RowPitch;
                const size_t rowbytes = static_cast<size_t>(g_grid_w) * sizeof(int16_t) * 2;
                int16_t *dst = g_of_cpu.data();
                for (uint32_t y = 0; y < g_grid_h; ++y)
                    std::memcpy(dst + static_cast<size_t>(y) * g_grid_w * 2,
                                src + static_cast<size_t>(y) * pitch, rowbytes);
            }
            g_executes++;
            if (g_executes == 1)
            {
                char buf[256] = {};
                snprintf(buf, sizeof(buf), "[d3d12-10] first execute OK: grid %ux%u", g_grid_w, g_grid_h);
                dbglog(buf);
            }
            // Vector telemetry every 60th execute: frozen vs live data?
            if ((g_executes % 60) == 1)
            {
                const size_t cx = g_grid_w / 2, cy = g_grid_h / 2;
                const size_t ci = (cy * g_grid_w + cx) * 2;
                const size_t li = ((g_grid_h - 1) * g_grid_w + (g_grid_w - 1)) * 2;
                uint64_t dlhash = 1469598103934665603ULL;
                const uint8_t *db = reinterpret_cast<const uint8_t *>(g_of_cpu.data());
                const size_t dbsz = g_of_cpu.size() * sizeof(int16_t);
                for (size_t bi = 0; bi < dbsz; bi += 64)
                {
                    dlhash ^= db[bi];
                    dlhash *= 1099511628211ULL;
                }
                char buf[512] = {};
                snprintf(buf, sizeof(buf),
                         "[d3d12-10] vec%llu: avg=%.1f c0=(%d,%d) cc=(%.1f,%.1f) c1=(%.1f,%.1f) dlhash=%llx "
                         "graysum=%llx",
                         static_cast<unsigned long long>(g_executes), g_flow_avg, g_of_cpu[0], g_of_cpu[1],
                         g_of_cpu[ci] / 32.0f, g_of_cpu[ci + 1] / 32.0f,
                         g_of_cpu[li] / 32.0f, g_of_cpu[li + 1] / 32.0f,
                         static_cast<unsigned long long>(dlhash), static_cast<unsigned long long>(sum));
                dbglog(buf);
            }
            have_vectors = true;
        }
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
    int last_of = 0;
    bool show_tex = false;
    uint64_t flow_srv = 0;
    uint32_t gw = 0, gh = 0;
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
        last_of = g_last_of_code;
        show_tex = g_show_tex;
        flow_srv = g_out_srv.handle;
        gw = g_grid_w;
        gh = g_grid_h;
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

    ImGui::TextUnformatted("NVOF Motion (phase 2b: vectors) [d3d12-10]");

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
    {
        bool synth = g_synth_test;
        if (ImGui::Checkbox("Synthetic +8px test (self-test)", &synth))
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_synth_test = synth;
        }
    }
    {
        bool show = show_tex;
        if (ImGui::Checkbox("Show flow texture (debug view)", &show))
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_show_tex = show;
        }
    }
    if (show_tex && flow_srv != 0 && gw > 0 && gh > 0)
    {
        // PPOM-style in-menu debug view: raw R16G16F (R/G = flow XY).
        const float disp_w = 430.0f;
        const float disp_h = disp_w * static_cast<float>(gh) / static_cast<float>(gw);
        ImGui::Image((ImTextureID)flow_srv, ImVec2(disp_w, disp_h));
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
    ImGui::Text("Last code: OF %d", last_of);
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
        // NOTE: loaded DLLs and native objects are intentionally
        // NOT torn down here; driver calls inside DllMain risk loader-lock
        // deadlocks. The OS reclaims everything on process exit.
        break;
    }

    return TRUE;
}
