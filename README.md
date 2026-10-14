# NVOF Motion – HW optical-flow motion vectors (ReShade add-on)

ReShade add-on that computes per-pixel motion vectors on the NVIDIA
fixed-function optical-flow engine (NVOFA, Turing+) for games/emulators
that do not provide their own (e.g. PCSX2). No ReShade recompile needed,
just drop `nvof_motion.addon64` next to `ReShade.dll`.

- **Add-on + probe sources:** [`NvofMotion/`](NvofMotion/) (see its
  [README](NvofMotion/README.md), [STATUS](NvofMotion/STATUS.md),
  [build guide](NvofMotion/BUILD.md), MIT
  [LICENSE](NvofMotion/LICENSE))
- **CI:** [`.github/workflows/build-nvof.yml`](.github/workflows/build-nvof.yml)
  builds `nvof_motion.addon64` + `nvof_probe.exe` on `windows-2022`.
- **History:** full `nvof-motion` bisect history lives in branch
  [`archive/nvof-motion-bisect`](https://github.com/berlint-hub/NVofMOTION/tree/archive/nvof-motion-bisect).

Status: **phase 2b + 2c working** – native D3D12 NvOF session (grid4/FAST),
R16G16F `MotionVectors` field + injection into `motion_vectors_tex`.
