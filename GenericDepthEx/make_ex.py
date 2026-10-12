#!/usr/bin/env python3
"""Build generic_depth_ex.cpp from clean crosire/reshade upstream.

Usage (CI):
  python GenericDepthEx/make_ex.py \
    --src reshade-main/examples/09-depth/generic_depth_addon.cpp \
    --dst GenericDepthEx/generic_depth_ex.cpp

Applies the Generic Depth Ex customizations intentionally used by this repo:
- lowest_vertices / lowest_drawcalls heuristics
- empty-stats guard
- reversed-depth filtering
- inverted copy logic
- config/UI entries and renamed addon metadata

Fails loudly if upstream drifted in a way that breaks the required anchors.
"""

import argparse
import sys


def replace_once(content, needle, replacement, label, src):
    if needle not in content:
        raise RuntimeError(f"FAIL: {label} not found in {src} (upstream drifted?)")
    if content.count(needle) != 1:
        raise RuntimeError(f"FAIL: {label} found {content.count(needle)}x in {src}")
    return content.replace(needle, replacement, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True)
    args = ap.parse_args()

    with open(args.src, "r", encoding="utf-8", newline="") as f:
        content = f.read()

    try:
        # 1. enum lowest heuristics
        content = replace_once(
            content,
            "\tprefer_vertices = 0,\n\tvertices,\n\tdrawcalls\n};",
            "\tprefer_vertices = 0,\n\tvertices,\n\tdrawcalls,\n\tlowest_vertices,\n\tlowest_drawcalls\n};",
            "draw_stats_heuristic enum",
            args.src,
        )

        # 2. s_reversed_filtering global
        content = replace_once(
            content,
            "static unsigned int s_custom_resolution_filtering[2] = {};\n",
            "static unsigned int s_custom_resolution_filtering[2] = {};\n// Filter by reversed depth buffer detection (0 = any, 1 = prefer reversed, 2 = exclude reversed)\nstatic unsigned int s_reversed_filtering = 0;\n",
            "s_reversed_filtering declaration",
            args.src,
        )

        # 3. operator> empty guard + lowest branches
        content = replace_once(
            content,
            "\tbool operator>(const draw_stats &other) const\n\t{\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::vertices)\n\t\t\treturn vertices > other.vertices;\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::drawcalls)\n\t\t\treturn drawcalls > other.drawcalls;\n\n\t\treturn (drawcalls_indirect < (drawcalls / 3) ?\n\t\t\t// Choose snapshot with the most vertices, since that is likely to contain the main scene\n\t\t\tvertices > other.vertices :\n\t\t\t// Or check draw calls, since vertices may not be accurate if application is using indirect draw calls\n\t\t\tdrawcalls > other.drawcalls);\n\t}",
            "\tbool operator>(const draw_stats &other) const\n\t{\n\t\tconst bool is_empty = vertices == 0 && drawcalls == 0;\n\t\tconst bool other_empty = other.vertices == 0 && other.drawcalls == 0;\n\n\t\tif (is_empty || other_empty)\n\t\t\treturn (!is_empty && other_empty) || (is_empty == other_empty && (vertices + drawcalls) > (other.vertices + other.drawcalls));\n\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::vertices)\n\t\t\treturn vertices > other.vertices;\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::drawcalls)\n\t\t\treturn drawcalls > other.drawcalls;\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::lowest_vertices)\n\t\t\treturn vertices < other.vertices;\n\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::lowest_drawcalls)\n\t\t\treturn drawcalls < other.drawcalls;\n\n\t\treturn (drawcalls_indirect < (drawcalls / 3) ?\n\t\t\t// Choose snapshot with the most vertices, since that is likely to contain the main scene\n\t\t\tvertices > other.vertices :\n\t\t\t// Or check draw calls, since vertices may not be accurate if application is using indirect draw calls\n\t\t\tdrawcalls > other.drawcalls);\n\t}",
            "draw_stats::operator> block",
            args.src,
        )

        # 4. inverted copy logic (suitable for current upstream block)
        content = replace_once(
            content,
            "\t\t\t\tif (s_force_clear_index.first == 0)\n\t\t\t\t{\n\t\t\t\t\t// Use greater equals operator here to handle case where the same scene is first rendered into a shadow map and then for real (e.g. Mirror's Edge main menu)\n\t\t\t\t\tdo_copy = current_stats.vertices >= state.best_copy_stats.vertices || (op == clear_op::fullscreen_draw && current_stats.drawcalls >= state.best_copy_stats.drawcalls);\n\t\t\t\}\n",
            "\t\t\t\tif (s_force_clear_index.first == 0)\n\t\t\t\t{\n\t\t\t\t\tif (s_draw_stats_heuristic == draw_stats_heuristic::lowest_vertices)\n\t\t\t\t\t\tdo_copy = current_stats.vertices <= state.best_copy_stats.vertices;\n\t\t\t\t\telse if (s_draw_stats_heuristic == draw_stats_heuristic::lowest_drawcalls)\n\t\t\t\t\t\tdo_copy = current_stats.drawcalls <= state.best_copy_stats.drawcalls;\n\t\t\t\t\telse\n\t\t\t\t\t{\n\t\t\t\t\t\t// Use greater equals operator here to handle case where the same scene is first rendered into a shadow map and then for real (e.g. Mirror's Edge main menu)\n\t\t\t\t\t\tdo_copy = current_stats.vertices >= state.best_copy_stats.vertices || (op == clear_op::fullscreen_draw && current_stats.drawcalls >= state.best_copy_stats.drawcalls);\n\t\t\t\t\t}\n\t\t\t\}\n",
            "clear logic block",
            args.src,
        )

        # 5. clamp heuristic and config load
        content = replace_once(
            content,
            "\treshade::get_config_value(nullptr, \"DEPTH\", \"DrawStatsHeuristic\", reinterpret_cast<unsigned int &>(s_draw_stats_heuristic));\n\treshade::get_config_value(nullptr, \"DEPTH\", \"UseAspectRatioHeuristics\", reinterpret_cast<unsigned int &>(s_aspect_ratio_heuristic));\n",
            "\treshade::get_config_value(nullptr, \"DEPTH\", \"DrawStatsHeuristic\", reinterpret_cast<unsigned int &>(s_draw_stats_heuristic));\n\tif (s_draw_stats_heuristic > draw_stats_heuristic::lowest_drawcalls)\n\t\ts_draw_stats_heuristic = draw_stats_heuristic::prefer_vertices;\n\treshade::get_config_value(nullptr, \"DEPTH\", \"UseAspectRatioHeuristics\", reinterpret_cast<unsigned int &>(s_aspect_ratio_heuristic));\n",
            "heuristic config load",
            args.src,
        )

        # 6. FilterReversed config load
        content = replace_once(
            content,
            "\treshade::get_config_value(nullptr, \"DEPTH\", \"FilterFormat\", s_format_filtering);\n\treshade::get_config_value(nullptr, \"DEPTH\", \"FilterResolutionWidth\", s_custom_resolution_filtering[0]);\n\treshade::get_config_value(nullptr, \"DEPTH\", \"FilterResolutionHeight\", s_custom_resolution_filtering[1]);\n",
            "\treshade::get_config_value(nullptr, \"DEPTH\", \"FilterFormat\", s_format_filtering);\n\treshade::get_config_value(nullptr, \"DEPTH\", \"FilterReversed\", s_reversed_filtering);\n\treshade::get_config_value(nullptr, \"DEPTH\", \"FilterResolutionWidth\", s_custom_resolution_filtering[0]);\n\treshade::get_config_value(nullptr, \"DEPTH\", \"FilterResolutionHeight\", s_custom_resolution_filtering[1]);\n",
            "FilterReversed config load",
            args.src,
        )

        # 7. reversed filter in selection loop
        content = replace_once(
            content,
            "\t\tif (s_aspect_ratio_heuristic != aspect_ratio_heuristic::none && !check_aspect_ratio(static_cast<float>(info.desc.texture.width), static_cast<float>(info.desc.texture.height), static_cast<float>(frame_width), static_cast<float>(frame_height)))\n\t\t\tcontinue; // Not a good fit\n",
            "\t\tif (s_aspect_ratio_heuristic != aspect_ratio_heuristic::none && !check_aspect_ratio(static_cast<float>(info.desc.texture.width), static_cast<float>(info.desc.texture.height), static_cast<float>(frame_width), static_cast<float>(frame_height)))\n\t\t\tcontinue; // Not a good fit\n\t\tif (s_reversed_filtering == 1 && !info.last_frame_stats.reversed_clear_value)\n\t\t\tcontinue;\n\t\tif (s_reversed_filtering == 2 && info.last_frame_stats.reversed_clear_value)\n\t\t\tcontinue;\n",
            "reversed depth filter selection",
            args.src,
        )

        # 8. heuristic combo items
        content = replace_once(
            content,
            "\tconst char *const draw_stats_heuristic_items[] = {\n\t\t\"Default\",\n\t\t\"Higher vertices\",\n\t\t\"Higher draw calls\"\n\t};",
            "\tconst char *const draw_stats_heuristic_items[] = {\n\t\t\"Default\",\n\t\t\"Higher vertices\",\n\t\t\"Higher draw calls\",\n\t\t\"Lowest vertices\",\n\t\t\"Lowest draw calls\"\n\t};",
            "draw_stats heuristic items",
            args.src,
        )

        # 9. reversed combo UI and depth format UI combined
        content = replace_once(
            content,
            "\tconst char *const depth_format_items[] = { // Needs to match switch in 'check_depth_format' above\n\t\t\"All\",\n\t\t\"D16  \",\n\t\t\"D16S8\",\n\t\t\"D24X8\",\n\t\t\"D24S8\",\n\t\t\"D32  \",\n\t\t\"D32S8\",\n\t\t\"INTZ \"\n\t};\n\tif (ImGui::Combo(\"Filter by depth buffer format\", reinterpret_cast<int *>(&s_format_filtering), depth_format_items, static_cast<int>(std::size(depth_format_items))))\n\t{\n\t\treshade::set_config_value(nullptr, \"DEPTH\", \"FilterFormat\", s_format_filtering);\n\t\tforce_reset = true;\n\t}\n",
            "\tconst char *const depth_format_items[] = { // Needs to match switch in 'check_depth_format' above\n\t\t\"All\",\n\t\t\"D16  \",\n\t\t\"D16S8\",\n\t\t\"D24X8\",\n\t\t\"D24S8\",\n\t\t\"D32  \",\n\t\t\"D32S8\",\n\t\t\"INTZ \"\n\t};\n\tif (ImGui::Combo(\"Filter by depth buffer format\", reinterpret_cast<int *>(&s_format_filtering), depth_format_items, static_cast<int>(std::size(depth_format_items))))\n\t{\n\t\treshade::set_config_value(nullptr, \"DEPTH\", \"FilterFormat\", s_format_filtering);\n\t\tforce_reset = true;\n\t}\n\n\tconst char *const reversed_filter_items[] = {\n\t\t\"Any\",\n\t\t\"Prefer reversed\",\n\t\t\"Exclude reversed\"\n\t};\n\tif (ImGui::Combo(\"Filter by reversed depth buffer\", reinterpret_cast<int *>(&s_reversed_filtering), reversed_filter_items, static_cast<int>(std::size(reversed_filter_items))))\n\t{\n\t\treshade::set_config_value(nullptr, \"DEPTH\", \"FilterReversed\", s_reversed_filtering);\n\t\tforce_reset = true;\n\t}\n",
            "depth format + reversed filter UI",
            args.src,
        )

        # 10. addon metadata rename
        content = replace_once(
            content,
            "extern \"C\" __declspec(dllexport) const char *NAME = \"Generic Depth\";\nextern \"C\" __declspec(dllexport) const char *DESCRIPTION = \"Automatic depth buffer detection that works in the majority of games.\";\n",
            "extern \"C\" __declspec(dllexport) const char *NAME = \"Generic Depth Ex\";\nextern \"C\" __declspec(dllexport) const char *DESCRIPTION = \"Extended depth buffer detection (lowest heuristics + reversed filter). Disable built-in 'Generic Depth' when using this.\";\n",
            "addon metadata",
            args.src,
        )

        with open(args.dst, "w", encoding="utf-8", newline="") as f:
            f.write(content)

        print(f"OK: wrote {args.dst} ({len(content)} chars)")
        return 0

    except RuntimeError as exc:
        print(str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())
