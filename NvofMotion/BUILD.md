# NVOF Motion – build guide

Standalone ReShade add-on (`nvof_motion.addon64`) + stock-recipe probe
(`nvof_probe.exe`) + debug effect. Everything builds in CI
(`.github/workflows/build-nvof.yml`, `windows-2022`); this guide is for
local builds and for anyone who wants to reuse the code.

## Runtime requirements

- Windows 10/11 x64, NVIDIA Turing+ GPU, driver shipping `nvofapi64.dll`
  (verified on RTX 4080, driver 617.14)
- D3D12 game (PCSX2 tested), ReShade 6.x with add-on support
- Backbuffer `R8G8B8A8_UNORM` (otherwise the add-on stays idle)

## Build prerequisites

- Visual Studio 2022 (MSVC v143) + CMake >= 3.24
- CUDA Toolkit 12.x (for `cuda.h` + `nvcc`; the add-on binds `nvcuda.dll`
  at runtime and links nothing, the probe links `cuda.lib`; installer sets
  `CUDA_PATH`, which CMake picks up)
- ReShade headers: `git clone https://github.com/crosire/reshade`
  (needs submodules for the overlay: `deps/imgui`)
- NvOF headers, API 5.0: `NvOFInterface/` from
  https://github.com/mbucchia/Optical-Flow-SDK
  (NVIDIA's own GitHub is stale at 2.0; the gated SDK zip from
  developer.nvidia.com works too)

## Local build

```bat
git clone https://github.com/berlint-hub/NVofMOTION
cd NVofMOTION
git clone https://github.com/crosire/reshade reshade-main
git clone https://github.com/mbucchia/Optical-Flow-SDK nvof-sdk
cmake -S NvofMotion -B build -A x64 ^
  "-DRESHADE_ROOT=%CD%/reshade-main" ^
  "-DNVOF_SDK=%CD%/nvof-sdk/NvOFInterface"
cmake --build build --config Release
```

Artifacts: `build/Release/nvof_motion.addon64` and (separate configure)
`NvofMotion/probe` -> `nvof_probe.exe` (see CI workflow for the exact
probe commands).

## Deploy & run

1. Copy `nvof_motion.addon64` next to `ReShade.dll` (or into `AddonPath`).
2. Copy `NvofMotion/fx/MotionVectors_Debug.fx` next to your other `.fx` files.
3. In-game: ReShade overlay -> enable the add-on, set **Target texture**
   to exactly `motion_vectors_tex`, enable the debug technique.
4. Overlay shows session/caps/counters/flow stats; `nvof_motion_debug.log`
   next to the addon has bring-up details and failure strings.

## Thanks

- **NVIDIA** – Optical Flow SDK (headers, samples, programming guide).
- **mbucchia (Matthieu Bucchioni)** – public Optical-Flow-SDK mirror.
  Without API 5.0 headers + stock samples this project would still be
  stuck on OF5.
- **crosire (ReShade)** – the add-on API and open runtime source (that's
  how the semantic texture binding was decoded).
- **OpenCV (`cudaoptflow`) and NVIDIA DALI teams** – production NvOF
  patterns (hint grids, stream order, stride handling) used as reference
  during the bisect.
- **PCSX2 team** – the host emulator everything is tested against.
- **Jimver** – the `cuda-toolkit` GitHub Action used by CI.
