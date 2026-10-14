# Forum post draft – NVOF Motion (ReShade forum, English)

Title: `[Addon] NVOF Motion – HW optical-flow motion vectors (NVOFA) for games without them`

---

Hi all,
I'd like to share a new open-source ReShade add-on: **NVOF Motion**.

**What it does:**
Computes per-pixel motion vectors on NVIDIA's fixed-function optical-flow
engine (NVOFA, Turing+) for games/emulators that don't provide their own.
My target is PCSX2 – PS2-era games have no velocity buffer, so TAA and
frame-gen shaders have nothing to work with. No ReShade recompile needed,
just drop `nvof_motion.addon64` next to `ReShade.dll`.

**How it works:**
- Grabs the D3D12 backbuffer each frame (non-blocking readback), converts
  to luma and feeds NVOFA through its native D3D12 interface (no CUDA).
- Grid-4 FAST session → R16G16F flow field in pixels, overlay stats
  (avg / moving / invalid %), plus a `MotionVectors` texture other shaders
  can consume (injection into your `motion_vectors_tex`).
- Driver "no match" sentinel vectors (INT16_MIN) are filtered to zero.
- Ships with a debug visualization effect (flow→color) and a standalone
  `nvof_probe.exe` stock-recipe sanity checker.

**Requirements:** Windows x64, NVIDIA Turing+ GPU, recent driver (tested
617.14), D3D12 game, ReShade 6.x.

**Status:** working on RTX 4080 + PCSX2 @3440x1440 (see screenshots:
overlay stats + color visualization). MIT licensed, builds in GitHub
Actions, build guide in repo.

Repo: https://github.com/berlint-hub/NVofMOTION

**Known limits:** D3D12 only, RGBA8 backbuffer, injection needs an exact
grid-size match, border vectors can be noisy (PS2 dithering).

Feedback and testers welcome – especially other GPUs (Ampere/Turing) and
other D3D12 games!
