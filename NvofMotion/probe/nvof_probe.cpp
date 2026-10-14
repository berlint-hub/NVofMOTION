// nvof_probe: stock-recipe NvOF execute check in a clean process.
// Synthetic gray frames (known shift), no images, no game, no ReShade.
// Build in CI (MSVC + CUDA toolkit headers), run on the rig, paste output.
// Exit 0 = execute SUCCESS on all cases, 1 = any failure.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <windows.h>

#include <cuda.h>

#include "nvOpticalFlowCommon.h"
#include "nvOpticalFlowCuda.h"

namespace
{

int g_failures = 0;

#define PROBE_CHECK_CU(call)                                                                  \
    do                                                                                        \
    {                                                                                         \
        const CUresult _cu = (call);                                                          \
        if (_cu != CUDA_SUCCESS)                                                              \
        {                                                                                     \
            std::printf("  CU FAIL %s -> %d\n", #call, static_cast<int>(_cu));                \
            ++g_failures;                                                                     \
            return false;                                                                     \
        }                                                                                     \
    } while (0)

#define PROBE_CHECK_OF(call)                                                                  \
    do                                                                                        \
    {                                                                                         \
        const NV_OF_STATUS _st = (call);                                                       \
        if (_st != NV_OF_SUCCESS)                                                              \
        {                                                                                     \
            std::printf("  OF FAIL %s -> %d\n", #call, static_cast<int>(_st));                \
            ++g_failures;                                                                     \
            return false;                                                                     \
        }                                                                                     \
    } while (0)

// Deterministic textured pattern (both axes gradients + checker), shifted by dx.
void fill_frame(std::vector<uint8_t> &img, int w, int h, int dx)
{
    img.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int sx = (x + w - (dx % w)) % w; // shift content by dx
            const uint8_t v =
                static_cast<uint8_t>((((sx * 255) / w) + ((y * 255) / h) + (((sx / 16 + y / 16) & 1) ? 64 : 0)) &
                                     0xFF);
            img[static_cast<size_t>(y) * w + x] = v;
        }
}

bool run_case(int W, int H)
{
    std::printf("CASE %dx%d (apiVer=0x%X)\n", W, H, static_cast<unsigned>(NV_OF_API_VERSION));

    PROBE_CHECK_CU(cuInit(0));
    int nGpu = 0;
    PROBE_CHECK_CU(cuDeviceGetCount(&nGpu));
    std::printf("  cuda devices: %d\n", nGpu);
    if (nGpu < 1)
    {
        ++g_failures;
        return false;
    }
    CUdevice dev = 0;
    PROBE_CHECK_CU(cuDeviceGet(&dev, 0));
    char name[128] = {};
    PROBE_CHECK_CU(cuDeviceGetName(name, sizeof(name), dev));
    std::printf("  device0: %s\n", name);

    CUcontext ctx = nullptr;
    PROBE_CHECK_CU(cuCtxCreate(&ctx, 0, dev));
    PROBE_CHECK_CU(cuCtxSetCurrent(ctx));

    HMODULE nvof = LoadLibraryW(L"nvofapi64.dll");
    if (nvof == nullptr)
    {
        std::printf("  LOAD FAIL nvofapi64.dll\n");
        ++g_failures;
        return false;
    }
    using CreateInstance_fn = NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST *);
    auto pCreate = reinterpret_cast<CreateInstance_fn>(GetProcAddress(nvof, "NvOFAPICreateInstanceCuda"));
    if (pCreate == nullptr)
    {
        std::printf("  PROC FAIL NvOFAPICreateInstanceCuda\n");
        ++g_failures;
        return false;
    }
    // Optional: driver max API level (informational).
    using MaxVer_fn = NV_OF_STATUS(NVOFAPI *)(uint32_t *);
    auto pMaxVer = reinterpret_cast<MaxVer_fn>(GetProcAddress(nvof, "NvOFGetMaxSupportedApiVersion"));
    if (pMaxVer != nullptr)
    {
        uint32_t maxVer = 0;
        if (pMaxVer(&maxVer) == NV_OF_SUCCESS)
            std::printf("  driver maxApiVer=0x%X\n", maxVer);
    }

    NV_OF_CUDA_API_FUNCTION_LIST api = {};
    PROBE_CHECK_OF(pCreate(NV_OF_API_VERSION, &api));

    NvOFHandle hOf = nullptr;
    PROBE_CHECK_OF(api.nvCreateOpticalFlowCuda(ctx, &hOf));
    // Stock default (AppOFCuda useCudaStream=0): SetIO unconditionally, null streams.
    PROBE_CHECK_OF(api.nvOFSetIOCudaStreams(hOf, nullptr, nullptr));

    NV_OF_INIT_PARAMS init = {};
    init.width = static_cast<uint32_t>(W);
    init.height = static_cast<uint32_t>(H);
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_1; // stock default gridSize=1
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM; // stock default preset
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    init.hPrivData = nullptr;
    PROBE_CHECK_OF(api.nvOFInit(hOf, &init));

    NV_OF_BUFFER_DESCRIPTOR idesc = {};
    idesc.width = static_cast<uint32_t>(W);
    idesc.height = static_cast<uint32_t>(H);
    idesc.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
    idesc.bufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    NvOFGPUBufferHandle inBufs[2] = {};
    CUarray inArrs[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        PROBE_CHECK_OF(
            api.nvOFCreateGPUBufferCuda(hOf, &idesc, NV_OF_CUDA_BUFFER_TYPE_CUARRAY, &inBufs[i]));
        inArrs[i] = api.nvOFGPUBufferGetCUarray(inBufs[i]);
        if (inArrs[i] == nullptr)
        {
            std::printf("  OF FAIL GetCUarray[%d] -> null\n", i);
            ++g_failures;
            return false;
        }
    }
    const uint32_t gw = (static_cast<uint32_t>(W) + 1 - 1) / 1;
    const uint32_t gh = (static_cast<uint32_t>(H) + 1 - 1) / 1;
    NV_OF_BUFFER_DESCRIPTOR odesc = {};
    odesc.width = gw;
    odesc.height = gh;
    odesc.bufferUsage = NV_OF_BUFFER_USAGE_OUTPUT;
    odesc.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;
    NvOFGPUBufferHandle outBuf = nullptr;
    PROBE_CHECK_OF(api.nvOFCreateGPUBufferCuda(hOf, &odesc, NV_OF_CUDA_BUFFER_TYPE_CUARRAY, &outBuf));
    CUarray outArr = api.nvOFGPUBufferGetCUarray(outBuf);
    if (outArr == nullptr)
    {
        std::printf("  OF FAIL GetCUarray[out] -> null\n");
        ++g_failures;
        return false;
    }

    // Synthetic pair: frame1 = frame0 shifted +8px in x.
    std::vector<uint8_t> f0, f1;
    fill_frame(f0, W, H, 0);
    fill_frame(f1, W, H, 8);
    CUarray dsts[2] = { inArrs[0], inArrs[1] };
    const uint8_t *srcs[2] = { f0.data(), f1.data() };
    for (int i = 0; i < 2; ++i)
    {
        CUDA_MEMCPY2D m = {};
        m.srcMemoryType = CU_MEMORYTYPE_HOST;
        m.srcHost = const_cast<uint8_t *>(srcs[i]);
        m.srcPitch = static_cast<size_t>(W);
        m.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        m.dstArray = dsts[i];
        m.WidthInBytes = static_cast<size_t>(W);
        m.Height = static_cast<size_t>(H);
        PROBE_CHECK_CU(cuMemcpy2DAsync(&m, nullptr));
    }
    PROBE_CHECK_CU(cuCtxSynchronize());

    // Stock order: input = older (buf0), reference = newer (buf1).
    NV_OF_EXECUTE_INPUT_PARAMS in = {};
    in.inputFrame = inBufs[0];
    in.referenceFrame = inBufs[1];
    in.disableTemporalHints = NV_OF_FALSE; // stock default
    NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
    out.outputBuffer = outBuf;
    const NV_OF_STATUS est = api.nvOFExecute(hOf, &in, &out);
    std::printf("  nvOFExecute -> %d\n", static_cast<int>(est));
    if (est != NV_OF_SUCCESS)
    {
        char err[256] = {};
        uint32_t errsz = sizeof(err);
        if (api.nvOFGetLastError != nullptr && api.nvOFGetLastError(hOf, err, &errsz) == NV_OF_SUCCESS &&
            err[0] != '\0')
            std::printf("  lastError: %s\n", err);
        ++g_failures;
    }
    else
    {
        PROBE_CHECK_CU(cuCtxSynchronize());
        std::vector<int16_t> host(static_cast<size_t>(gw) * gh * 2, 0);
        CUDA_MEMCPY2D d = {};
        d.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        d.srcArray = outArr;
        d.dstMemoryType = CU_MEMORYTYPE_HOST;
        d.dstHost = host.data();
        d.dstPitch = static_cast<size_t>(gw) * 4;
        d.WidthInBytes = static_cast<size_t>(gw) * 4;
        d.Height = gh;
        PROBE_CHECK_CU(cuMemcpy2DAsync(&d, nullptr));
        PROBE_CHECK_CU(cuCtxSynchronize());
        // S10.5 fixed point -> float pixels; expect ~+8px in x at center.
        const size_t cx = gw / 2, cy = gh / 2;
        const size_t ci = (cy * gw + cx) * 2;
        std::printf("  center vec: x=%.2f y=%.2f (raw %d %d)\n", host[ci] / 32.0f, host[ci + 1] / 32.0f,
                    host[ci], host[ci + 1]);
    }

    api.nvOFDestroyGPUBufferCuda(outBuf);
    api.nvOFDestroyGPUBufferCuda(inBufs[0]);
    api.nvOFDestroyGPUBufferCuda(inBufs[1]);
    api.nvOFDestroy(hOf);
    FreeLibrary(nvof);
    cuCtxSetCurrent(nullptr);
    cuCtxDestroy(ctx);
    return g_failures == 0;
}

} // namespace

// Addon-mimicking case: bring-up on MAIN thread (with detach, like the addon
// build), then the whole frame (attach/upload/execute/download/detach) on a
// WORKER thread (like the addon's game thread). Decides threading/context
// migration as the OF5 trigger.
bool run_case_threaded(int W, int H)
{
    std::printf("CASE-THREADED %dx%d (apiVer=0x%X)\n", W, H, static_cast<unsigned>(NV_OF_API_VERSION));

    PROBE_CHECK_CU(cuInit(0));
    CUdevice dev = 0;
    PROBE_CHECK_CU(cuDeviceGet(&dev, 0));
    CUcontext ctx = nullptr;
    PROBE_CHECK_CU(cuCtxCreate(&ctx, 0, dev));
    PROBE_CHECK_CU(cuCtxSetCurrent(ctx));

    HMODULE nvof = LoadLibraryW(L"nvofapi64.dll");
    if (nvof == nullptr)
    {
        std::printf("  LOAD FAIL nvofapi64.dll\n");
        ++g_failures;
        return false;
    }
    using CreateInstance_fn = NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST *);
    auto pCreate = reinterpret_cast<CreateInstance_fn>(GetProcAddress(nvof, "NvOFAPICreateInstanceCuda"));
    if (pCreate == nullptr)
    {
        std::printf("  PROC FAIL NvOFAPICreateInstanceCuda\n");
        ++g_failures;
        return false;
    }
    NV_OF_CUDA_API_FUNCTION_LIST api = {};
    PROBE_CHECK_OF(pCreate(NV_OF_API_VERSION, &api));
    NvOFHandle hOf = nullptr;
    PROBE_CHECK_OF(api.nvCreateOpticalFlowCuda(ctx, &hOf));
    PROBE_CHECK_OF(api.nvOFSetIOCudaStreams(hOf, nullptr, nullptr));

    NV_OF_INIT_PARAMS init = {};
    init.width = static_cast<uint32_t>(W);
    init.height = static_cast<uint32_t>(H);
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_1;
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    PROBE_CHECK_OF(api.nvOFInit(hOf, &init));

    NV_OF_BUFFER_DESCRIPTOR idesc = {};
    idesc.width = static_cast<uint32_t>(W);
    idesc.height = static_cast<uint32_t>(H);
    idesc.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
    idesc.bufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    NvOFGPUBufferHandle inBufs[2] = {};
    CUarray inArrs[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        PROBE_CHECK_OF(
            api.nvOFCreateGPUBufferCuda(hOf, &idesc, NV_OF_CUDA_BUFFER_TYPE_CUARRAY, &inBufs[i]));
        inArrs[i] = api.nvOFGPUBufferGetCUarray(inBufs[i]);
        if (inArrs[i] == nullptr)
        {
            std::printf("  OF FAIL GetCUarray[%d] -> null\n", i);
            ++g_failures;
            return false;
        }
    }
    NV_OF_BUFFER_DESCRIPTOR odesc = {};
    odesc.width = static_cast<uint32_t>(W);
    odesc.height = static_cast<uint32_t>(H);
    odesc.bufferUsage = NV_OF_BUFFER_USAGE_OUTPUT;
    odesc.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;
    NvOFGPUBufferHandle outBuf = nullptr;
    PROBE_CHECK_OF(api.nvOFCreateGPUBufferCuda(hOf, &odesc, NV_OF_CUDA_BUFFER_TYPE_CUARRAY, &outBuf));
    CUarray outArr = api.nvOFGPUBufferGetCUarray(outBuf);
    if (outArr == nullptr)
    {
        std::printf("  OF FAIL GetCUarray[out] -> null\n");
        ++g_failures;
        return false;
    }
    PROBE_CHECK_CU(cuCtxSetCurrent(nullptr)); // DETACH on main (addon build detaches)

    std::vector<uint8_t> f0, f1;
    fill_frame(f0, W, H, 0);
    fill_frame(f1, W, H, 8);

    // Worker = addon game thread: attach, upload, execute, download, detach.
    std::thread worker([&]() {
        if (cuCtxSetCurrent(ctx) != CUDA_SUCCESS)
        {
            std::printf("  [worker] set-current FAILED\n");
            ++g_failures;
            return;
        }
        CUarray dsts[2] = { inArrs[0], inArrs[1] };
        const uint8_t *srcs[2] = { f0.data(), f1.data() };
        for (int i = 0; i < 2; ++i)
        {
            CUDA_MEMCPY2D m = {};
            m.srcMemoryType = CU_MEMORYTYPE_HOST;
            m.srcHost = const_cast<uint8_t *>(srcs[i]);
            m.srcPitch = static_cast<size_t>(W);
            m.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            m.dstArray = dsts[i];
            m.WidthInBytes = static_cast<size_t>(W);
            m.Height = static_cast<size_t>(H);
            if (cuMemcpy2DAsync(&m, nullptr) != CUDA_SUCCESS)
            {
                std::printf("  [worker] upload[%d] FAILED\n", i);
                ++g_failures;
                return;
            }
        }
        if (cuCtxSynchronize() != CUDA_SUCCESS)
        {
            std::printf("  [worker] sync FAILED\n");
            ++g_failures;
            return;
        }
        NV_OF_EXECUTE_INPUT_PARAMS in = {};
        in.inputFrame = inBufs[0]; // older (stock order)
        in.referenceFrame = inBufs[1];
        in.disableTemporalHints = NV_OF_FALSE;
        NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
        out.outputBuffer = outBuf;
        const NV_OF_STATUS est = api.nvOFExecute(hOf, &in, &out);
        std::printf("  [worker] nvOFExecute -> %d\n", static_cast<int>(est));
        if (est != NV_OF_SUCCESS)
        {
            ++g_failures;
            return;
        }
        if (cuCtxSynchronize() != CUDA_SUCCESS)
        {
            std::printf("  [worker] post-sync FAILED\n");
            ++g_failures;
            return;
        }
        std::vector<int16_t> host(static_cast<size_t>(W) * H * 2, 0);
        CUDA_MEMCPY2D d = {};
        d.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        d.srcArray = outArr;
        d.dstMemoryType = CU_MEMORYTYPE_HOST;
        d.dstHost = host.data();
        d.dstPitch = static_cast<size_t>(W) * 4;
        d.WidthInBytes = static_cast<size_t>(W) * 4;
        d.Height = static_cast<size_t>(H);
        if (cuMemcpy2DAsync(&d, nullptr) != CUDA_SUCCESS || cuCtxSynchronize() != CUDA_SUCCESS)
        {
            std::printf("  [worker] download FAILED\n");
            ++g_failures;
            return;
        }
        const size_t ci = ((H / 2) * W + (W / 2)) * 2;
        std::printf("  [worker] center vec: x=%.2f y=%.2f\n", host[ci] / 32.0f, host[ci + 1] / 32.0f);
        cuCtxSetCurrent(nullptr); // DETACH (addon frame detaches)
    });
    worker.join();

    api.nvOFDestroyGPUBufferCuda(outBuf);
    api.nvOFDestroyGPUBufferCuda(inBufs[0]);
    api.nvOFDestroyGPUBufferCuda(inBufs[1]);
    api.nvOFDestroy(hOf);
    FreeLibrary(nvof);
    cuCtxDestroy(ctx);
    return true;
}

int main()
{
    std::printf("nvof_probe (stock recipe, synthetic frames)\n");
    run_case(640, 480);
    run_case_threaded(640, 480);
    run_case(3440, 1440);
    run_case_threaded(3440, 1440);
    if (g_failures == 0)
        std::printf("PROBE RESULT: PASS\n");
    else
        std::printf("PROBE RESULT: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
