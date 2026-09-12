#!/usr/bin/env python3
"""Bake the authored Control Center layout into a fixed firmware table."""

import argparse
import json
from pathlib import Path


ORIENTATIONS = ("portrait", "landscape")
EXPECTED_CANVASES = {"portrait": (368, 448), "landscape": (448, 368)}
REQUIRED_IDS = (
    "grabber",
    "header",
    "wifi",
    "bluetooth",
    "link",
    "volume",
    "brightness",
    "notifications_header",
    "library_notification",
    "controller_notification",
)
INTERACTIVE_IDS = REQUIRED_IDS[2:7] + REQUIRED_IDS[8:]


def fail(message):
    raise ValueError(f"gen_control_center_layout.py: {message}")


def overlaps(first, second):
    ax, ay, aw, ah = first
    bx, by, bw, bh = second
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def validate(document):
    if not isinstance(document, dict):
        fail("document root must be an object")
    if document.get("schema_version") != 1 or document.get("screen") != "control_center":
        fail("unsupported Control Center schema")
    if document.get("element_order") != list(REQUIRED_IDS):
        fail("element_order must contain the stable Control Center IDs in schema order")
    orientations = document.get("orientations")
    if not isinstance(orientations, dict) or set(orientations) != set(ORIENTATIONS):
        fail("orientations must contain exactly portrait and landscape")

    validated = {}
    for orientation in ORIENTATIONS:
        authored = orientations[orientation]
        canvas = EXPECTED_CANVASES[orientation]
        if not isinstance(authored, dict) or authored.get("canvas") != list(canvas):
            fail(f"{orientation}.canvas must be {list(canvas)}")
        source_rects = authored.get("rects")
        if not isinstance(source_rects, dict) or set(source_rects) != set(REQUIRED_IDS):
            fail(f"{orientation}.rects must contain exactly the stable Control Center IDs")
        rects = {}
        for element_id in REQUIRED_IDS:
            rect = source_rects[element_id]
            if not isinstance(rect, list) or len(rect) != 4 or any(type(value) is not int for value in rect):
                fail(f"{orientation}.{element_id} must be four integers")
            x, y, width, height = rect
            if x < 0 or y < 0 or width <= 0 or height <= 0 or x + width > canvas[0] or y + height > canvas[1]:
                fail(f"{orientation}.{element_id} leaves the canvas")
            if element_id in INTERACTIVE_IDS and (width < 44 or height < 44):
                fail(f"{orientation}.{element_id} is smaller than the 44px tap target")
            rects[element_id] = tuple(rect)
        for index, first_id in enumerate(REQUIRED_IDS):
            for second_id in REQUIRED_IDS[index + 1 :]:
                if overlaps(rects[first_id], rects[second_id]):
                    fail(f"{orientation}.{first_id} overlaps {second_id}")
        validated[orientation] = (canvas, rects)
    return validated


def enum_name(element_id):
    return "CONTROL_CENTER_ELEMENT_" + element_id.upper()


def generate(document):
    layouts = validate(document)
    lines = [
        "/* GENERATED FILE - do not edit.",
        " * python tools/gen_control_center_layout.py main/ui/control_center_layout.json",
        " *     main/ui/control_center_layout_generated.h",
        " */",
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
            f"    CONTROL_CENTER_ELEMENT_COUNT = {len(REQUIRED_IDS)}",
            "} control_center_element_id_t;",
            "",
            "typedef struct { int16_t x, y, width, height; } control_center_layout_rect_t;",
            "typedef struct {",
            "    int16_t canvas_width, canvas_height;",
            "    control_center_layout_rect_t rects[CONTROL_CENTER_ELEMENT_COUNT];",
            "} control_center_layout_t;",
            "",
        ]
    )
    for orientation in ORIENTATIONS:
        canvas, rects = layouts[orientation]
        lines.append(f"static const control_center_layout_t control_center_layout_{orientation} = {{")
        lines.append(f"    .canvas_width = {canvas[0]}, .canvas_height = {canvas[1]},")
        lines.append("    .rects = {")
        for element_id in REQUIRED_IDS:
            lines.append(f"        [{enum_name(element_id)}] = {{{', '.join(map(str, rects[element_id]))}}},")
        lines.extend(["    },", "};", ""])
    return "\n".join(lines).rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        baked = generate(json.loads(args.source.read_text(encoding="utf-8")))
        if args.check:
            if args.output.read_text(encoding="utf-8") != baked:
                parser.exit(1, f"{args.output} is stale; regenerate it\n")
        else:
            args.output.write_text(baked, encoding="utf-8", newline="\n")
    except (OSError, json.JSONDecodeError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
