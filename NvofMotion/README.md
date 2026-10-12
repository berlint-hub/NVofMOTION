# NVOF Motion – HW optical-flow motion vectors (x64 only)

ReShade add-on that computes per-pixel motion vectors on the NVIDIA
fixed-function optical-flow engine (NVOFA, Turing+) for games/emulators
that do not provide their own (e.g. PCSX2). No ReShade recompile needed,
just drop `nvof_motion.addon64` next to `ReShade.dll`.

> Status: **phase 2a (frame capture + CUDA transport, no execute yet)** –
> backbuffer is copied into shared slot textures with our own native D3D12
> list, fenced, and moved into NvOF input arrays. Overlay shows counters.

## Roadmap

- **1a (done):** build + load + overlay + probe `nvofapi64.dll` /
  `NvOFAPICreateInstanceCuda`, status reporting. No CUDA toolkit needed.
- **1b (done):** CUDA context (LUID-matched to the game adapter), NvOF
  session create/init, `nvOFGetCaps` in the overlay.
- **2a (this):** lazy session at real frame size + rebuild on resize, own
  native command list + fence -> CUDA semaphore, per-frame copy + memcpy
  into NvOF input arrays, overlay counters.
- **2b:** `NvOFExecute`, PTX int16->float convert, R16G16F output +
  `MotionVectors` binding / configurable `TargetTexture` injection, preview.

## Requirements

- Windows x64, D3D12 game (v1 supports D3D12 only, silent elsewhere)
- NVIDIA Turing+ GPU + driver with NvOF (driver 445.87+; ships `nvofapi64.dll`
  and `nvcuda.dll`; both bound explicitly at runtime, no .lib linkage)
- Backbuffer R8G8B8A8 (otherwise idle with status)
- On AMD/Intel or without those DLLs the add-on stays idle (see overlay)

## Install & config

1. Put `nvof_motion.addon64` next to `ReShade.dll` (or into `AddonPath`).
2. In the overlay it shows up under the add-ons list as **NVOF Motion**.
3. Kill switch (also survives restart):
   ```ini
   [NVOF]
   Enable=1
   ```
   Set to `0` to fully disable (tears down an active session immediately).

## Build in CI

Workflow `.github/workflows/build-nvof.yml` (`windows-2022`, x64 Release):
- checkout `crosire/reshade` into `reshade-main` (headers + `deps/imgui`)
- checkout `NVIDIA/NVIDIAOpticalFlowSDK` into `nvof-sdk` (BSD headers)
- install the CUDA toolkit (for `cuda.h`; types only, nothing linked)
- CMake + upload artifact `nvof_motion.addon64`
