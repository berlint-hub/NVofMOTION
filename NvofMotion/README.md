# NVOF Motion – HW optical-flow motion vectors (x64 only)

ReShade add-on that computes per-pixel motion vectors on the NVIDIA
fixed-function optical-flow engine (NVOFA, Turing+) for games/emulators
that do not provide their own (e.g. PCSX2). No ReShade recompile needed,
just drop `nvof_motion.addon64` next to `ReShade.dll`.

> Status: **phase 1a (probe skeleton)** – validates build chain, loading,
> overlay and driver-runtime probing. No frame processing yet.

## Roadmap

- **1a (this):** build + load + overlay + probe `nvofapi64.dll` /
  `NvOFAPICreateInstanceCuda`, status reporting. No CUDA toolkit needed.
- **1b:** CUDA context, NvOF session create/destroy, `nvOFGetCaps`, overlay
  shows caps (max size, grid sizes, perf levels).
- **2:** frame capture (own shared D3D12 textures) -> `NvOFExecute` on a
  worker thread -> R16G16F motion-vector texture exposed to shaders
  (PPOM-style `MotionInput` injection), overlay preview.

## Requirements

- Windows x64, D3D12 game (v1 supports D3D12 only, silent elsewhere)
- NVIDIA Turing+ GPU + driver with NvOF (driver 445.87+; ships `nvofapi64.dll`)
- On AMD/Intel or without the driver DLL the add-on stays idle (see overlay)

## Install & config

1. Put `nvof_motion.addon64` next to `ReShade.dll` (or into `AddonPath`).
2. In the overlay it shows up under the add-ons list as **NVOF Motion**.
3. Kill switch (also survives restart):
   ```ini
   [NVOF]
   Enable=1
   ```
   Set to `0` to fully disable.

## Build in CI

Workflow `.github/workflows/build-nvof.yml` (`windows-2022`, x64 Release):
- checkout `crosire/reshade` into `reshade-main` (headers + `deps/imgui`)
- CMake + upload artifact `nvof_motion.addon64`
- Phase 1b will additionally check out `NVIDIA/NVIDIAOpticalFlowSDK`
  into `nvof-sdk` and install the CUDA toolkit.
