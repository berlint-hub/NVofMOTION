#!/usr/bin/env python3
"""Build generic_depth_ex.cpp from clean crosire/reshade upstream.

Usage (CI):
  python GenericDepthEx/make_ex.py \
    --src reshade-main/examples/09-depth/generic_depth_addon.cpp \
    --dst GenericDepthEx/generic_depth_ex.cpp

Applies: lowest_vertices/lowest_drawcalls heuristics, empty-stats guard,
inverted copy logic, FilterReversed config + UI, NAME rename to Generic Depth Ex.
Fails loudly if upstream drifted (pattern not found).

Line endings: the upstream checkout may use CRLF (e.g. actions/checkout on
windows-2022 with core.autocrlf=true) or LF. Input is normalized to LF via
universal newlines and output is always written as LF, so the same patterns
work regardless of platform.
"""

import argparse
import sys

REPLACEMENTS = [
    # 1. enum lowest heuristics
    (
        "prefer_vertices = 0,\n\tvertices,\n\tdrawcalls\n};",
        "prefer_vertices = 0,\n\tvertices,\n\tdrawcalls,\n\tlowest_vertices,\n\tlowest_drawcalls\n};",
    ),
    # 2. s_reversed_filtering global
    (
        "static unsigned int s_custom_resolution_filtering[2] = {};\n",
        "static unsigned int s_custom_resolution_filtering[2] = {};\n// Filter by reversed depth buffer detection (0 = any, 1 = prefer reversed, 2 = exclude reversed)\nstatic unsigned int s_reversed_filtering = 0;\n",
    ),
    # 3a. operator> empty guard (unique context with function head)
    (
        "\tbool operator>(const draw_stats &other) const\n\t{\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::vertices)",
        "\tbool operator>(const draw_stats &other) const\n\t{\n\t\tconst bool is_empty = vertices == 0 && drawcalls == 0;\n\t\tconst bool other_empty = other.vertices == 0 && other.drawcalls == 0;\n\t\tif (is_empty != other_empty)\n\t\t\treturn other_empty; // Any real stats are better than empty, regardless of heuristic\n\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::vertices)",
    ),
    # 3b. lowest branches (unique: drawcalls branch + closing brace + empty line + return)
    (
        "\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::drawcalls)\n\t\t\treturn drawcalls > other.drawcalls;\n\n\t\treturn (drawcalls_indirect",
        "\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::drawcalls)\n\t\t\treturn drawcalls > other.drawcalls;\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::lowest_vertices)\n\t\t\treturn vertices < other.vertices;\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::lowest_drawcalls)\n\t\t\treturn drawcalls < other.drawcalls;\n\n\t\treturn (drawcalls_indirect",
    ),
    # 4. inverted copy logic (full block incl. original do_copy line)
    (
        "\t\t\tif (s_force_clear_index.first == 0)\n\t\t\t{\n\t\t\t\t// Use greater equals operator here to handle case where the same scene is first rendered into a shadow map and then for real (e.g. Mirror's Edge main menu)\n\t\t\t\tdo_copy = current_stats.vertices >= state.best_copy_stats.vertices || (op == clear_op::fullscreen_draw && current_stats.drawcalls >= state.best_copy_stats.drawcalls);\n\t\t\t}",
        "\t\t\tif (s_force_clear_index.first == 0)\n\t\t\t{\n\t\t\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::lowest_vertices || s_draw_stats_heuristic == draw_stats_heuristic::lowest_drawcalls)\n\t\t\t\t{\n\t\t\t\t\tconst bool best_empty = state.best_copy_stats.vertices == 0 && state.best_copy_stats.drawcalls == 0;\n\t\t\t\t\t// Inverted logic for lowest heuristics: keep the smallest workload (e.g. for UI / small depth buffers)\n\t\t\t\t\tdo_copy = best_empty || current_stats.vertices <= state.best_copy_stats.vertices || (op == clear_op::fullscreen_draw && current_stats.drawcalls <= state.best_copy_stats.drawcalls);\n\t\t\t\t}\n\t\t\t\telse\n\t\t\t\t{\n\t\t\t\t\t// Use greater equals operator here to handle case where the same scene is first rendered into a shadow map and then for real (e.g. Mirror's Edge main menu)\n\t\t\t\t\tdo_copy = current_stats.vertices >= state.best_copy_stats.vertices || (op == clear_op::fullscreen_draw && current_stats.drawcalls >= state.best_copy_stats.drawcalls);\n\t\t\t\t}\n\t\t\t}",
    ),
    # 5a. clamp heuristic after config load
    (
        'reshade::get_config_value(nullptr, "DEPTH", "DrawStatsHeuristic", reinterpret_cast<unsigned int &>(s_draw_stats_heuristic));\n',
        'reshade::get_config_value(nullptr, "DEPTH", "DrawStatsHeuristic", reinterpret_cast<unsigned int &>(s_draw_stats_heuristic));\n\tif (s_draw_stats_heuristic > draw_stats_heuristic::lowest_drawcalls)\n\t\ts_draw_stats_heuristic = draw_stats_heuristic::prefer_vertices;\n',
    ),
    # 5b. FilterReversed config load
    (
        'reshade::get_config_value(nullptr, "DEPTH", "FilterFormat", s_format_filtering);\n',
        'reshade::get_config_value(nullptr, "DEPTH", "FilterFormat", s_format_filtering);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterReversed", s_reversed_filtering);\n',
    ),
    # 6. reversed filter in selection loop (full aspect_ratio check context)
    (
        "\t\tif (s_aspect_ratio_heuristic != aspect_ratio_heuristic::none && !check_aspect_ratio(static_cast<float>(info.desc.texture.width), static_cast<float>(info.desc.texture.height), static_cast<float>(frame_width), static_cast<float>(frame_height)))\n\t\t\tcontinue; // Not a good fit\n",
        "\t\tif (s_aspect_ratio_heuristic != aspect_ratio_heuristic::none && !check_aspect_ratio(static_cast<float>(info.desc.texture.width), static_cast<float>(info.desc.texture.height), static_cast<float>(frame_width), static_cast<float>(frame_height)))\n\t\t\tcontinue; // Not a good fit\n\n\t\t// Filter by reversed depth buffer detection\n\t\tconst bool is_reversed = info.last_frame_stats.reversed_clear_value;\n\t\tif (s_reversed_filtering == 1 && !is_reversed)\n\t\t\tcontinue; // Only allow reversed\n\t\tif (s_reversed_filtering == 2 && is_reversed)\n\t\t\tcontinue; // Exclude reversed\n",
    ),
    # 7. heuristic combo items
    (
        '\t\t"Default",\n\t\t"Higher vertices",\n\t\t"Higher draw calls"\n\t};',
        '\t\t"Default",\n\t\t"Higher vertices",\n\t\t"Higher draw calls",\n\t\t"Lowest vertices",\n\t\t"Lowest draw calls"\n\t};',
    ),
    # 8. reversed combo UI (full FilterFormat combo block)
    (
        '\tif (ImGui::Combo("Filter by depth buffer format", reinterpret_cast<int *>(&s_format_filtering), depth_format_items, static_cast<int>(std::size(depth_format_items))))\n\t{\n\t\treshade::set_config_value(nullptr, "DEPTH", "FilterFormat", s_format_filtering);\n\t\tforce_reset = true;\n\t}\n',
        '\tif (ImGui::Combo("Filter by depth buffer format", reinterpret_cast<int *>(&s_format_filtering), depth_format_items, static_cast<int>(std::size(depth_format_items))))\n\t{\n\t\treshade::set_config_value(nullptr, "DEPTH", "FilterFormat", s_format_filtering);\n\t\tforce_reset = true;\n\t}\n\n\tconst char *const reversed_items[] = {\n\t\t"Any",\n\t\t"Prefer reversed",\n\t\t"Exclude reversed"\n\t};\n\tif (ImGui::Combo("Filter by reversed depth", reinterpret_cast<int *>(&s_reversed_filtering), reversed_items, static_cast<int>(std::size(reversed_items))))\n\t{\n\t\treshade::set_config_value(nullptr, "DEPTH", "FilterReversed", s_reversed_filtering);\n\t\tforce_reset = true;\n\t}\n',
    ),
    # 9a. NAME rename
    (
        'extern "C" __declspec(dllexport) const char *NAME = "Generic Depth";',
        'extern "C" __declspec(dllexport) const char *NAME = "Generic Depth Ex";',
    ),
    # 9b. DESCRIPTION rename
    (
        'extern "C" __declspec(dllexport) const char *DESCRIPTION = "Automatic depth buffer detection that works in the majority of games.";',
        'extern "C" __declspec(dllexport) const char *DESCRIPTION = "Extended depth buffer detection (lowest heuristics + reversed filter). Disable built-in \'Generic Depth\' when using this.";',
    ),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True)
    args = ap.parse_args()
    # newline=None enables universal newlines: CRLF/CR are translated to LF,
    # so patterns below always match no matter how git checked the file out.
    with open(args.src, "r", encoding="utf-8", newline=None) as f:
        content = f.read()
    for i, (old, new) in enumerate(REPLACEMENTS):
        if old not in content:
            print(f"FAIL: pattern {i} not found in {args.src} (upstream drifted?)")
            return 1
        if content.count(old) != 1:
            print(f"WARN: pattern {i} found {content.count(old)}x, replacing all")
        content = content.replace(old, new)
    # Always write LF for a deterministic output on every platform.
    with open(args.dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(
        f"OK: wrote {args.dst} ({len(content)} chars, {len(REPLACEMENTS)} replacements)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
