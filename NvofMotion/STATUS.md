# NVOF Motion – project status & handoff

> For continuing this work (human or AI): hardware is RTX 4080 + PCSX2 DX12
> + ReShade 6.8.0. No local toolchain anywhere – everything builds in GitHub
> Actions (`build-nvof.yml`, windows-2022, x64), everything is tested on the
> owner's rig (overlay screenshots + `ReShade.log`). Language: Czech.

## Goal

HW motion vectors via NVIDIA NVOFA for games/emulators that provide none
(PCSX2). Standalone `nvof_motion.addon64`, no ReShade recompile.
Sibling project (DONE, released): GenericDepthEx – see
`berlint-hub/Depth-buffer_extended` + forum post draft in chat history.
Related prior work: `berlint-hub/PPOM` (finds a game's own velocity
buffer – impossible on PCSX2, PS2-era games have none).

## Architecture (validated)

- Everything bound explicitly at runtime (`GetProcAddress` on `nvcuda.dll`
  + `nvofapi64.dll`); no `.lib` linkage. Silent idle on AMD/Intel/no driver.
- CUDA context LUID-matched to the game D3D12 adapter; own native D3D12
  command list + fence (never touches ReShade's lists).
- Transport: backbuffer (pre-effects, PRESENT assumed) -> CPU readback
  buffer -> fence poll with zero timeout (never stalls; busy frames skip)
  -> HtoD into NvOF input arrays -> sampled checksum (`Scene activity`).
  Zero-copy import (`cuImportExternalMemory`) failed with CU 201 – follow-up.
- Vectors intentionally lag ~2 frames (history pairing for TAA-style use).
- SEH guard around NvOF bring-up: driver crash -> status with stage + code.
- Session builds lazily at real frame size (aligned up to 8), rebuilt on
  resize (drain via `wait_idle` only when live objects exist – never on
  first build; `wait_idle` on a fresh device risks stall/deadlock).
- Output: SHORT2 CUARRAY -> host -> CPU int16->float (S10.5, scale 1/32)
  -> half -> R16G16F output texture + `MotionVectors` binding + optional
  injection into configurable `TargetTexture` (default `motion_vectors_tex`).
- Config `[NVOF]`: `Enable=1`, `FlowScale` (default 1/32), `TargetTexture`.
  Grid 4, fast preset, cost plane off.

## Done & verified on hardware

- 1a: build/load/overlay/`nvofapi64.dll` probe. 1b: ctx/session/caps
  (`32x32..8192x8192`). 2a: capture + checksums, resize rebuild.
- 2b plumbing: PTX kernel exists but is UNUSED (CPU convert instead);
  `flow_convert.cu` + CMake `nvcc -ptx` step stay green for a fast path.
- CI gotchas: `Jimver/cuda-toolkit` needs tag **v0.2.36+** (v0.2.11 does not
  know CUDA 12.6.0) + full semver (`'12.6.0'`, not `'12.6'`). Public NvOF
  headers are Turing-era only (DX12 interface is login-walled); old
  `apiVer` works on new drivers. `make_ex.py`-style CRLF lesson from the
  depth project applies to any pattern matching (normalize to LF).

## CURRENT BLOCKER: `nvOFExecute` returns OF 5 (INVALID_PARAM)

- `executes` stays 0, everything else flows (frames == memcpys, no crash).
- Ruled out: CUDEVICEPTR output (switched to CUARRAY, still OF 5),
  temporal hints (disabled, still OF 5).
- IN TEST NOW (overlay title tag `[roi1]` proves the binary):
  `numRois=1` + legal ROI `{0,0,(w/128)*128,(h/32)*32}`.
  Rationale for ROI test: all other params verified; `numRois=0` handling
  is the least documented part. If it fixes OF 5, decide shipping form
  (cropped ROI vs finding why 0 fails).
- Next theories if ROI fails: ABGR8 input format at execute time, swapped
  input/reference semantics, `apiVer 0x20` vs new driver, ROI align edge
  cases. Reference implementation: NVIDIA/DALI `optical_flow_impl`.
- Overlay `out grids mask 0x1` legend is misleading (grid 4 provably works);
  fix the text when touching the overlay next.

## Open follow-ups (not blockers)

- Zero-copy import (CU 201) to replace the ~1-3 ms readback roundtrip.
- `MotionVectors` binding consumer test (QuantMotion/DLSS feed shaders).
- Flow scale 1/32 correctness check against real screen motion.
- Forum post for Depth-buffer_extended release (draft exists in chat).
