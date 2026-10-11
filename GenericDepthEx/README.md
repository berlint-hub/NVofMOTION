# GenericDepthEx – rozšířený depth addon (x64 only)

Standalone fork `examples/09-depth/generic_depth_addon.cpp` z crosire/reshade.
Nebuildí celý ReShade, jen `generic_depth_ex.addon64`.

Rozšíření proti built-inu:
- `Draw stats heuristic`: `Lowest vertices`, `Lowest draw calls`
- `Filter by reversed depth`: `Any / Prefer reversed / Exclude reversed` (`[DEPTH] FilterReversed`)
- prázdné staty vždy prohrávají (ochrana proti UI/mini bufferům)

## Použití

1. V `ReShade.ini` vypni built-in, jinak poběží detekce 2x:
   ```ini
   [ADDON]
   DisabledAddons=Generic Depth
   ```
2. `generic_depth_ex.addon64` hoď vedle `ReShade.dll` (nebo do `AddonPath`).
3. V overlayi je addon jako `Generic Depth Ex`.

## Build lokálně

Potřebuje `../reshade-main` (include + deps/imgui):

```
cmake -S . -B build -A x64 -DRESHADE_ROOT=E:\Workspace\Reshade\reshade-main
cmake --build build --config Release
```

Výstup: `build/Release/generic_depth_ex.addon64`

## Build v CI (TEST repo)

Repo neobsahuje 70KB `generic_depth_ex.cpp`, generuje se z čistého upstreamu:
- `GenericDepthEx/make_ex.py` + `GenericDepthEx/CMakeLists.txt` + workflow
- workflow `.github/workflows/build-genericdepth.yml`:
  - `windows-2022`, jen x64 Release
  - checkout crosire/reshade do `reshade-main` (kvůli `include/` + `deps/imgui` + upstream cpp)
  - `python GenericDepthEx/make_ex.py --src reshade-main/... --dst GenericDepthEx/generic_depth_ex.cpp`
  - CMake + upload artifact `generic_depth_ex.addon64`
