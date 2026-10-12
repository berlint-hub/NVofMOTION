# NVOF Motion – HW optical-flow motion vectors (x64 only)

ReShade add-on that computes per-pixel motion vectors on the NVIDIA
fixed-function optical-flow engine (NVOFA, Turing+) for games/emulators
that do not provide their own (e.g. PCSX2). No ReShade recompile needed,
just drop `nvof_motion.addon64` next to `ReShade.dll`.

> Status: **phase 2b (vectors)** – per-frame `NvOFExecute`, PTX int16->float
> convert, R16G16F output texture + `MotionVectors` binding / configurable
> `TargetTexture` injection, flow stats in overlay.

## Roadmap

- **1a (done):** build + load + overlay + probe `nvofapi64.dll` /
  `NvOFAPICreateInstanceCuda`, status reporting. No CUDA toolkit needed.
- **1b (done):** CUDA context (LUID-matched to the game adapter), NvOF
  session create/init, `nvOFGetCaps` in the overlay.
- **2a (done):** lazy session at real frame size + rebuild on resize, own
  native command list + fence, readback capture + HtoD into NvOF inputs,
  checksum liveness proof in overlay.
- **2b (this):** `NvOFExecute`, PTX int16->float convert, R16G16F output +
  `MotionVectors` binding / configurable `TargetTexture` injection, stats.

## Shader interface

- **Binding:** `texture MotionTex < source = "MotionVectors"; >` gives the
  R16G16F flow field (grid 4 → texture is W/4 x H/4, values in pixels).
- **Injection:** `[NVOF] TargetTexture=<name>` (default `motion_vectors_tex`,
  empty = off) copies the field into that effect texture variable every frame
  (must exist as R16G16F at grid size, e.g. PPOM-style `pooled=false`).
- **Scale:** S10.5 hardware units -> px, `[NVOF] FlowScale` (default 1/32),
  tunable in overlay. Vectors lag ~2 frames by design (history pairing).

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
- install the CUDA toolkit (for `cuda.h` + `nvcc`; types and PTX only,
  nothing linked)
- CMake (PTX kernel -> embedded header) + upload artifact `nvof_motion.addon64`
