// convert_flow kernel: NVOF S10.5 fixed-point vectors -> float pixels.
// Built to PTX by CI (nvcc -ptx); driver JITs it, no arch coupling.
// Input:  short2 array (flowx, flowy in S10.5), n elements.
// Output: float2 array, scaled by `scale` (host-tunable, default 1/32).
// The host converts float -> R16G16F on CPU and uploads to the texture.

extern "C" __global__ void convert_flow(const short2 *inVec, float2 *outVec, int n, float scale)
{
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < n)
    {
        const short2 v = inVec[i];
        outVec[i] = make_float2(static_cast<float>(v.x) * scale, static_cast<float>(v.y) * scale);
    }
}
