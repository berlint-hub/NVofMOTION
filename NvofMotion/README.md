# NVOF Motion – HW optical-flow motion vectors (x64 only)

ReShade add-on that computes per-pixel motion vectors on the NVIDIA
fixed-function optical-flow engine (NVOFA, Turing+) for games/emulators
that do not provide their own (e.g. PCSX2). No ReShade recompile needed,
just drop `nvof_motion.addon64` next to `ReShade.dll`.

> Status: **phase 2b (vectors)** – per-frame `NvOFExecute`, PTX int16->float
> convert, R16G16F output texture + `MotionVectors` binding / configurable
> `TargetTexture` injection, flow stats in overlay.

## Roadmap

- **CUDA era (retired):** `NvOFAPICreateInstanceCuda` bring-up, LUID-matched
  context, HtoD capture – deterministic OF5 on every execute in the
  D3D12-active process (19 bisect builds `[streams]`, `[dali0]`-`[dali19]`;
  stock recipe passes in the clean `nvof_probe.exe`). See STATUS.md.
- **D3D12 backend (this):** native `NvOFAPICreateInstanceD3D12` session on
  the game device – DEFAULT R8_UNORM inputs + R16G16_SINT grid output,
  UPLOAD/READBACK staging, app/ofa fences (stock AppOFD3D12 discipline).
  No CUDA, no `nvcuda.dll`.

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
- NVIDIA Turing+ GPU + driver with NvOF (ships `nvofapi64.dll`, bound
  explicitly at runtime, no .lib linkage)
- Backbuffer R8G8B8A8 (otherwise idle with status)
- On AMD/Intel or without those DLLs the add-on stays idle (see overlay)

## License

MIT – see `LICENSE`. This directory contains only original code (addon,
probe, debug effect, docs). Build-time dependencies are fetched by CI,
not redistributed: ReShade headers (BSD-3-Clause, crosire/reshade) and
NVIDIA Optical Flow SDK headers, API 5.0 (NVIDIA LicenseAgreement,
via public mirror – see workflow file).

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
- checkout `mbucchia/Optical-Flow-SDK` `NvOFInterface` into `nvof-sdk` (API 5.0)
- install the CUDA toolkit (probe links `cuda.lib`; addon itself is CUDA-free)
- CMake + upload artifacts `nvof_motion.addon64`, `nvof_probe.exe`
