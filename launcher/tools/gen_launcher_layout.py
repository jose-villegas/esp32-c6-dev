#!/usr/bin/env python3
"""Bake launcher layout JSON into a zero-cost C geometry table.

    python tools/gen_launcher_layout.py main/ui/launcher_layout.json \
        main/ui/launcher_layout_generated.h

The JSON is the authored source of truth. Firmware includes only the generated
header: it performs no parsing, solving, allocation, or scaling.
"""

import argparse
import json
from pathlib import Path


ORIENTATIONS = ("portrait", "landscape")
EXPECTED_CANVASES = {"portrait": (368, 448), "landscape": (448, 368)}
CARD_IDS = ("last_played", "library", "render_lab")
REQUIRED_IDS = ("status_bar", *CARD_IDS, "page_indicator")


def fail(message):
    raise ValueError(f"gen_launcher_layout.py: {message}")


def read_rect(value, orientation, element_id):
    if not isinstance(value, list) or len(value) != 4:
        fail(f"{orientation}.{element_id} must be [x, y, width, height]")
    if any(type(component) is not int for component in value):
        fail(f"{orientation}.{element_id} components must be integers")
    x, y, width, height = value
    if x < 0 or y < 0 or width <= 0 or height <= 0:
        fail(f"{orientation}.{element_id} has an invalid rectangle")
    return tuple(value)


def overlaps(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def validate(document):
    if not isinstance(document, dict):
        fail("document root must be an object")
    if document.get("schema_version") != 1:
        fail("schema_version must be 1")
    if document.get("screen") != "launcher":
        fail("screen must be launcher")
    if document.get("element_order") != list(REQUIRED_IDS):
        fail("element_order must contain the stable launcher IDs in schema order")

    orientations = document.get("orientations")
    if not isinstance(orientations, dict) or set(orientations) != set(ORIENTATIONS):
        fail("orientations must contain exactly portrait and landscape")

    validated = {}
    for orientation in ORIENTATIONS:
        authored = orientations[orientation]
        if not isinstance(authored, dict):
            fail(f"{orientation} must be an object")
        expected_canvas = EXPECTED_CANVASES[orientation]
        if authored.get("canvas") != list(expected_canvas):
            fail(f"{orientation}.canvas must be {list(expected_canvas)}")

        authored_rects = authored.get("rects")
        if not isinstance(authored_rects, dict) or set(authored_rects) != set(REQUIRED_IDS):
            fail(f"{orientation}.rects must contain exactly the stable launcher IDs")

        rects = {
            element_id: read_rect(authored_rects[element_id], orientation, element_id)
            for element_id in REQUIRED_IDS
        }
        canvas_width, canvas_height = expected_canvas
        for element_id, (x, y, width, height) in rects.items():
            if x + width > canvas_width or y + height > canvas_height:
                fail(f"{orientation}.{element_id} leaves the canvas")

        for index, first_id in enumerate(CARD_IDS):
            first = rects[first_id]
            if first[2] < 44 or first[3] < 44:
                fail(f"{orientation}.{first_id} is smaller than the 44px tap target")
            for second_id in CARD_IDS[index + 1 :]:
                if overlaps(first, rects[second_id]):
                    fail(f"{orientation}.{first_id} overlaps {second_id}")

        status_bottom = rects["status_bar"][1] + rects["status_bar"][3]
        if status_bottom > min(rects[element_id][1] for element_id in CARD_IDS):
            fail(f"{orientation}.status_bar overlaps the app region")
        cards_bottom = max(rects[element_id][1] + rects[element_id][3] for element_id in CARD_IDS)
        if rects["page_indicator"][1] < cards_bottom:
            fail(f"{orientation}.page_indicator must be below every app card")

        validated[orientation] = (expected_canvas, rects)
    return validated


def enum_name(element_id):
    return "LAUNCHER_ELEMENT_" + element_id.upper()


def generate(document):
    layouts = validate(document)
    lines = [
        "/*=============================================================================",
        " * GENERATED FILE - do not edit.",
        " *",
        " *     python tools/gen_launcher_layout.py main/ui/launcher_layout.json \\",
        " *         main/ui/launcher_layout_generated.h",
        " *",
        " * Authored in main/ui/launcher_layout.json. Firmware reads this fixed",
        " * geometry directly; no layout parser or solver is linked on the device.",
        " *===========================================================================*/",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef enum {",
    ]
    for index, element_id in enumerate(REQUIRED_IDS):
        lines.append(f"    {enum_name(element_id)} = {index},")
    lines.extend(
        [
            f"    LAUNCHER_ELEMENT_COUNT = {len(REQUIRED_IDS)}",
            "} launcher_element_id_t;",
            "",
            "typedef struct {",
            "    int16_t x;",
            "    int16_t y;",
            "    int16_t width;",
            "    int16_t height;",
            "} launcher_layout_rect_t;",
            "",
            "typedef struct {",
            "    int16_t canvas_width;",
            "    int16_t canvas_height;",
            "    launcher_layout_rect_t rects[LAUNCHER_ELEMENT_COUNT];",
            "} launcher_layout_t;",
            "",
        ]
    )
    for orientation in ORIENTATIONS:
        canvas, rects = layouts[orientation]
        lines.append(f"static const launcher_layout_t launcher_layout_{orientation} = {{")
        lines.append(f"    .canvas_width = {canvas[0]},")
        lines.append(f"    .canvas_height = {canvas[1]},")
        lines.append("    .rects = {")
        for element_id in REQUIRED_IDS:
            x, y, width, height = rects[element_id]
            lines.append(
                f"        [{enum_name(element_id)}] = "
                f"{{{x}, {y}, {width}, {height}}},"
            )
        lines.extend(["    },", "};", ""])
    return "\n".join(lines).rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if output differs instead of writing it",
    )
    args = parser.parse_args()

    try:
        document = json.loads(args.source.read_text(encoding="utf-8"))
        baked = generate(document)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        parser.exit(1, f"{error}\n")

    if args.check:
        try:
            existing = args.output.read_text(encoding="utf-8")
        except OSError as error:
            parser.exit(1, f"{error}\n")
        if existing != baked:
            parser.exit(1, f"{args.output} is stale; regenerate it\n")
        return

    args.output.write_text(baked, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
