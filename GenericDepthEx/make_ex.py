#!/usr/bin/env python3
"""Build generic_depth_ex.cpp from clean crosire/reshade upstream.

Usage (CI):
  python GenericDepthEx/make_ex.py \
    --src reshade-main/examples/09-depth/generic_depth_addon.cpp \
    --dst GenericDepthEx/generic_depth_ex.cpp

Applies: lowest_vertices/lowest_drawcalls heuristics, empty-stats guard,
inverted copy logic, FilterReversed config + UI, custom resolution RANGE filter
(min/max width + height), NAME rename to Generic Depth Ex.
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
    # 10. aspect enum: custom resolution RANGE mode (appended last, value 5)
    (
        "match_resolution_exactly,\n\tmatch_custom_resolution_exactly\n};",
        "match_resolution_exactly,\n\tmatch_custom_resolution_exactly,\n\tmatch_custom_resolution_range\n};",
    ),
    # 11. range filter bounds (0 = no bound on that side)
    (
        "static unsigned int s_format_filtering = 0;\n",
        "static unsigned int s_format_filtering = 0;\n// Custom width/height range filter (0 = no bound on that side)\nstatic unsigned int s_custom_resolution_range_min[2] = {};\nstatic unsigned int s_custom_resolution_range_max[2] = {};\n",
    ),
    # 12. range check in check_aspect_ratio
    (
        "\tif (s_aspect_ratio_heuristic == aspect_ratio_heuristic::match_custom_resolution_exactly)\n\t\treturn width_to_check == s_custom_resolution_filtering[0] && height_to_check == s_custom_resolution_filtering[1];\n",
        "\tif (s_aspect_ratio_heuristic == aspect_ratio_heuristic::match_custom_resolution_exactly)\n\t\treturn width_to_check == s_custom_resolution_filtering[0] && height_to_check == s_custom_resolution_filtering[1];\n\tif (s_aspect_ratio_heuristic == aspect_ratio_heuristic::match_custom_resolution_range)\n\t\treturn (s_custom_resolution_range_min[0] == 0 || width_to_check >= s_custom_resolution_range_min[0]) &&\n\t\t\t(s_custom_resolution_range_max[0] == 0 || width_to_check <= s_custom_resolution_range_max[0]) &&\n\t\t\t(s_custom_resolution_range_min[1] == 0 || height_to_check >= s_custom_resolution_range_min[1]) &&\n\t\t\t(s_custom_resolution_range_max[1] == 0 || height_to_check <= s_custom_resolution_range_max[1]);\n",
    ),
    # 13. range config load + extend upstream clamp to the new last value
    (
        '\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionWidth", s_custom_resolution_filtering[0]);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionHeight", s_custom_resolution_filtering[1]);\n\n\tif (s_aspect_ratio_heuristic > aspect_ratio_heuristic::match_custom_resolution_exactly)\n\t\ts_aspect_ratio_heuristic = aspect_ratio_heuristic::similar_aspect_ratio;\n',
        '\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionWidth", s_custom_resolution_filtering[0]);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionHeight", s_custom_resolution_filtering[1]);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionMinWidth", s_custom_resolution_range_min[0]);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionMaxWidth", s_custom_resolution_range_max[0]);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionMinHeight", s_custom_resolution_range_min[1]);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterResolutionMaxHeight", s_custom_resolution_range_max[1]);\n\n\tif (s_aspect_ratio_heuristic > aspect_ratio_heuristic::match_custom_resolution_range)\n\t\ts_aspect_ratio_heuristic = aspect_ratio_heuristic::similar_aspect_ratio;\n',
    ),
    # 14. range entry in the aspect ratio combo
    (
        '\t\t"Match resolution exactly",\n\t\t"Match custom width and height exactly"\n\t};',
        '\t\t"Match resolution exactly",\n\t\t"Match custom width and height exactly",\n\t\t"Match custom width and height range"\n\t};',
    ),
    # 16. per-runtime last auto-refresh stamp
    (
        "\t// The current depth shader resource view bound to shaders\n\t// This can be created from either the selected depth-stencil resource (if it supports shader access) or from a backup resource\n\tresource_view selected_shader_resource = { 0 };\n",
        "\t// The current depth shader resource view bound to shaders\n\t// This can be created from either the selected depth-stencil resource (if it supports shader access) or from a backup resource\n\tresource_view selected_shader_resource = { 0 };\n\n\t// Frame index of the last automatic selection refresh\n\tuint64_t last_auto_refresh_frame = 0;\n",
    ),
    # 17. auto-refresh interval (frames, 0 = off)
    (
        "static bool s_disable_intz = false;\n",
        "static bool s_disable_intz = false;\n// Automatically restart depth detection every N frames, so a stuck selection after scene changes is re-picked (0 = off)\nstatic unsigned int s_auto_refresh_frames = 600;\n",
    ),
    # 18. auto-refresh config load
    (
        'reshade::get_config_value(nullptr, "DEPTH", "FilterFormat", s_format_filtering);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterReversed", s_reversed_filtering);\n',
        'reshade::get_config_value(nullptr, "DEPTH", "FilterFormat", s_format_filtering);\n\treshade::get_config_value(nullptr, "DEPTH", "FilterReversed", s_reversed_filtering);\n\treshade::get_config_value(nullptr, "DEPTH", "AutoRefreshFrames", s_auto_refresh_frames);\n',
    ),
    # 19. auto-refresh: tear down selection so the normal flow re-picks + rebuilds
    # (the existing destroy at the end of on_begin_render_effects frees the old view exactly once)
    (
        "\tconst resource_view prev_shader_resource = data.selected_shader_resource;\n",
        "\tconst resource_view prev_shader_resource = data.selected_shader_resource;\n\n\t// Periodic auto-refresh (skipped while a manual override is active)\n\tif (s_auto_refresh_frames != 0 && data.override_depth_stencil == 0 && data.selected_depth_stencil != 0 &&\n\t\tdevice_data->frame_index - data.last_auto_refresh_frame >= s_auto_refresh_frames)\n\t{\n\t\tdata.last_auto_refresh_frame = device_data->frame_index;\n\t\tdevice_data->untrack_depth_stencil(device, data.selected_depth_stencil);\n\t\tdata.using_backup_texture = false;\n\t\tdata.selected_depth_stencil = { 0 };\n\t\tdata.selected_shader_resource = { 0 };\n\t}\n",
    ),
    # 20. auto-refresh UI
    (
        '\t\treshade::set_config_value(nullptr, "DEPTH", "FilterReversed", s_reversed_filtering);\n\t\tforce_reset = true;\n\t}\n',
        '\t\treshade::set_config_value(nullptr, "DEPTH", "FilterReversed", s_reversed_filtering);\n\t\tforce_reset = true;\n\t}\n\tif (ImGui::InputInt("Auto refresh every N frames (0 = off)", reinterpret_cast<int *>(&s_auto_refresh_frames)))\n\t{\n\t\treshade::set_config_value(nullptr, "DEPTH", "AutoRefreshFrames", s_auto_refresh_frames);\n\t\tforce_reset = true;\n\t}\n',
    ),
    # 15. range UI (min/max pairs, 0 = no limit)
    (
        '\tif (s_aspect_ratio_heuristic == aspect_ratio_heuristic::match_custom_resolution_exactly)\n\t{\n\t\tif (ImGui::InputInt2("Filter by width and height", reinterpret_cast<int *>(s_custom_resolution_filtering)))\n\t\t{\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionWidth", s_custom_resolution_filtering[0]);\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionHeight", s_custom_resolution_filtering[1]);\n\t\t\tforce_reset = true;\n\t\t}\n\t}\n',
        '\tif (s_aspect_ratio_heuristic == aspect_ratio_heuristic::match_custom_resolution_exactly)\n\t{\n\t\tif (ImGui::InputInt2("Filter by width and height", reinterpret_cast<int *>(s_custom_resolution_filtering)))\n\t\t{\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionWidth", s_custom_resolution_filtering[0]);\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionHeight", s_custom_resolution_filtering[1]);\n\t\t\tforce_reset = true;\n\t\t}\n\t}\n\tif (s_aspect_ratio_heuristic == aspect_ratio_heuristic::match_custom_resolution_range)\n\t{\n\t\tif (ImGui::InputInt2("Filter by min width and height (0 = no limit)", reinterpret_cast<int *>(s_custom_resolution_range_min)))\n\t\t{\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionMinWidth", s_custom_resolution_range_min[0]);\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionMinHeight", s_custom_resolution_range_min[1]);\n\t\t\tforce_reset = true;\n\t\t}\n\t\tif (ImGui::InputInt2("Filter by max width and height (0 = no limit)", reinterpret_cast<int *>(s_custom_resolution_range_max)))\n\t\t{\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionMaxWidth", s_custom_resolution_range_max[0]);\n\t\t\treshade::set_config_value(nullptr, "DEPTH", "FilterResolutionMaxHeight", s_custom_resolution_range_max[1]);\n\t\t\tforce_reset = true;\n\t\t}\n\t}\n',
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
