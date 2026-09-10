#!/usr/bin/env python3
"""Generate an icons_<name>.h from a PNG atlas plus its JSON manifest.

    python tools/gen_icons.py design/icons/system.png design/icons/system.json \\
        > main/gfx/icons_system.h

General-purpose, not system-set-specific: the same generator later produces
an app's own apps/<name>/icons_<name>.h from apps/<name>/icons/<name>.png +
.json (see docs/plans/Icon-Baker-Plan.md, "Ownership") - only the paths
differ. The emitted prefix (icon_<prefix>_*) is never a flag; it is always
the manifest's own filename stem, so system.json can only ever produce
icon_system_*.

STANDARD LIBRARY ONLY - NO PILLOW

Unlike gen_font.py and gen_boot_anim_image.py, this generator does not import
Pillow even lazily: Pillow is not installed in this environment (confirmed by
tools/screenshot.py, which used to carry a Pillow-conditional PNG branch that
never once fired on this machine and was rewritten to encode with zlib +
struct alone), and icon art has no proportional-advance or antialiasing need
that would justify the dependency. `read_png()` below decodes a PNG by hand:
walk the chunks, zlib.decompress() the IDAT stream, and unfilter every
scanline (all five PNG filter types, even though this repo's own bootstrapped
artwork only ever emits filter 0 - a PNG a paint program exports will not be
so tidy, and silently mis-decoding filter 3 would produce an atlas nobody
drew). Supports 8-bit grayscale/RGB/RGBA/palette and 1/2/4-bit
grayscale/palette (the depths PNG allows for those color types); 16-bit
depth and Adam7 interlacing are rejected outright rather than guessed at, as
neither is a realistic export for flat pixel-art icons.

STRICTLY 1BPP: NO THRESHOLD

Every pixel must decode to fully-opaque pure black (ink, "on") or
fully-opaque pure white (background, "off"). Anything else - a grey
antialiased edge, partial transparency - is rejected with the offending
pixel's coordinates rather than rounded toward whichever side a threshold
guesses. These are pixel art: an in-between pixel means the artist exported
wrong, not that the generator should decide for them.

THE MANIFEST

A JSON object naming the atlas's cell geometry and one name per occupied
cell:

    {
        "cell_w": 16, "cell_h": 16,
        "icons": [ { "name": "check", "col": 0, "row": 0 } ]
    }

The PNG's own width/height must be an exact multiple of cell_w/cell_h - the
grid's row/column count is derived from that division, not carried
separately in the manifest, so the two files cannot disagree about it.

WHAT GETS REJECTED BEFORE ANYTHING IS EMITTED

Following gen_boot_anim_image.py/gen_font.py's own convention: every check
below runs to completion, and the header is only written once none of them
have called die() - see this file's own main() for the full order. Checked:
every pixel strictly on or off; the PNG's dimensions divide evenly into
whole cells; every named cell non-empty; every non-empty cell named; names
unique and valid C identifiers; each icon's run count at or under
RUN_COUNT_CAP; and, per icon, that packing its pixels into rows[] and
unpacking them again reproduces the exact same on/off grid the PNG decoded
to - the honest self-check for a bit-packing routine, proving the ROUND TRIP
rather than re-deriving the same bits with different code and comparing (see
docs/plans/Icon-Baker-Plan.md, "Do not assert the baked bytes against a
Python re-implementation of the packer").
"""

import json
import re
import struct
import sys
import textwrap
import zlib
from pathlib import Path

# ui.h's UI_DRAW_BITMAP_MAX_BLOCKS: ui_draw_bitmap()'s stack buffer today.
# An icon whose baked run count exceeds this cannot be drawn through that
# path without overflowing it - see this file's own top comment on why the
# cap is checked here, at bake time, rather than left for a caller to find
# by overflowing a stack array.
RUN_COUNT_CAP = 48

IDENTIFIER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def die(msg):
    sys.exit("gen_icons.py: %s" % msg)


# --- PNG decoding (stdlib only) ---------------------------------------------

def _paeth(a, b, c):
    """PaethPredictor(left, above, upper-left) - PNG spec section 9.4,
    verbatim: picks whichever of a/b/c is closest to a linear predictor of
    the other two, the filter type most edges in real artwork end up using."""
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(data, height, stride, bpp):
    """Reverses PNG's per-scanline filtering (spec section 9) - `data` is
    the raw, already-decompressed IDAT stream: (1 filter-type byte + stride
    data bytes) per row. `bpp` is bytes-per-pixel for filtering purposes
    (ceil(bitdepth*channels/8), minimum 1 - PNG's own rule, not a guess).
    Returns the unfiltered scanlines concatenated, stride bytes each, with
    no filter-type bytes left in them."""
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = data[pos]
        pos += 1
        row = bytearray(data[pos:pos + stride])
        pos += stride
        if len(row) != stride:
            die("scanline %d is short: got %d bytes, expected %d - the "
                "decompressed IDAT stream is truncated" % (y, len(row), stride))
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + a) & 0xFF
        elif ftype == 2:  # Up
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 0xFF
        elif ftype == 3:  # Average
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                c = prev[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + _paeth(a, prev[x], c)) & 0xFF
        else:
            die("scanline %d uses filter type %d, which does not exist "
                "(PNG defines 0-4) - the IDAT stream is corrupt" % (y, ftype))
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return bytes(out)


def _unpack_samples(row_bytes, width, bitdepth, channels):
    """One scanline's raw bytes -> width*channels integer samples, 0..(2**bitdepth-1)
    each. Depths under 8 only occur for single-channel color types (PNG's own
    rule - grayscale or palette), so bit-packing never has to split a sample
    across channels."""
    if bitdepth == 8:
        return list(row_bytes[:width * channels])
    samples = []
    mask = (1 << bitdepth) - 1
    per_byte = 8 // bitdepth
    needed = width * channels
    bi = 0
    while len(samples) < needed:
        byte = row_bytes[bi]
        bi += 1
        for shift in range(per_byte - 1, -1, -1):
            if len(samples) >= needed:
                break
            samples.append((byte >> (shift * bitdepth)) & mask)
    return samples


def read_png(path):
    """Decodes a PNG into (width, height, pixels) where pixels[y][x] is an
    (r, g, b, a) tuple, 0..255 each. Handles bit depths 1/2/4/8 for
    grayscale/palette and 8 for RGB/RGBA (the depths PNG allows for each
    color type) - see this file's own top comment for what is deliberately
    NOT supported and why."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        die("%s is not a PNG (bad signature)" % path)

    pos = 8
    width = height = bitdepth = color_type = interlace = None
    idat = bytearray()
    palette = None
    trns = None

    while pos < len(data):
        if pos + 8 > len(data):
            die("%s: truncated chunk header at byte %d" % (path, pos))
        length, = struct.unpack_from(">I", data, pos)
        ctype = data[pos + 4:pos + 8]
        cdata = data[pos + 8:pos + 8 + length]
        pos += 8 + length + 4  # skip the trailing CRC32

        if ctype == b"IHDR":
            (width, height, bitdepth, color_type, _comp, _filt, interlace) = \
                struct.unpack(">IIBBBBB", cdata)
        elif ctype == b"PLTE":
            if len(cdata) % 3 != 0:
                die("%s: PLTE chunk length %d is not a multiple of 3" %
                    (path, len(cdata)))
            palette = [tuple(cdata[i:i + 3]) for i in range(0, len(cdata), 3)]
        elif ctype == b"tRNS":
            trns = cdata
        elif ctype == b"IDAT":
            idat += cdata
        elif ctype == b"IEND":
            break

    if width is None:
        die("%s: no IHDR chunk found" % path)
    if interlace != 0:
        die("%s: interlaced (Adam7) PNGs are not supported - re-export "
            "non-interlaced" % path)
    if bitdepth == 16:
        die("%s: 16-bit channel depth is not supported for flat pixel-art "
            "icons - re-export at 8-bit or lower" % path)
    if bitdepth not in (1, 2, 4, 8):
        die("%s: unsupported bit depth %d" % (path, bitdepth))

    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in channels_by_type:
        die("%s: unsupported PNG color type %d" % (path, color_type))
    channels = channels_by_type[color_type]
    if color_type in (2, 4, 6) and bitdepth != 8:
        die("%s: color type %d requires 8-bit depth, got %d" %
            (path, color_type, bitdepth))
    if color_type == 3 and palette is None:
        die("%s: palette color type but no PLTE chunk" % path)

    raw = zlib.decompress(bytes(idat))
    stride = (width * channels * bitdepth + 7) // 8
    bpp = max(1, (channels * bitdepth + 7) // 8)
    unfiltered = _unfilter(raw, height, stride, bpp)

    maxval = (1 << bitdepth) - 1
    pixels = []
    for y in range(height):
        row_bytes = unfiltered[y * stride:(y + 1) * stride]
        samples = _unpack_samples(row_bytes, width, bitdepth, channels)
        row = []
        for x in range(width):
            s = samples[x * channels:(x + 1) * channels]
            if color_type == 0:  # grayscale
                v = s[0] * 255 // maxval
                a = 255
                if trns is not None and len(trns) >= 2:
                    key, = struct.unpack(">H", trns[:2])
                    if s[0] == key:
                        a = 0
                row.append((v, v, v, a))
            elif color_type == 2:  # RGB
                row.append((s[0], s[1], s[2], 255))
            elif color_type == 3:  # palette
                idx = s[0]
                if idx >= len(palette):
                    die("%s: pixel (%d,%d) uses palette index %d, only %d "
                        "entries" % (path, x, y, idx, len(palette)))
                r, g, b = palette[idx]
                a = trns[idx] if trns is not None and idx < len(trns) else 255
                row.append((r, g, b, a))
            elif color_type == 4:  # grayscale + alpha
                row.append((s[0], s[0], s[0], s[1]))
            else:  # RGBA
                row.append((s[0], s[1], s[2], s[3]))
        pixels.append(row)

    return width, height, pixels


# --- 1bpp validation ---------------------------------------------------------

def pixel_to_bit(rgba, x, y, path):
    """A pixel is strictly "on" (opaque black) or "off" (opaque white) -
    anything else (grey, colour, partial alpha) means the source was not
    genuinely 1bpp art, and this dies naming exactly where."""
    r, g, b, a = rgba
    if (r, g, b, a) == (0, 0, 0, 255):
        return True
    if (r, g, b, a) == (255, 255, 255, 255):
        return False
    die("%s: pixel (%d,%d) is RGBA%r - neither opaque black (ink) nor "
        "opaque white (background). This generator rejects anti-aliasing "
        "and transparency rather than guessing a threshold - re-export "
        "with pure black/white and no partial alpha." % (path, x, y, rgba))


# --- packing -----------------------------------------------------------------

def pack_icon(bits, w, h):
    """bits[y][x] (bool) -> (row_bytes, stride). MSB is column 0 - the same
    convention icons.h's own icon_check_bitmap already documents."""
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            if bits[y][x]:
                out[y * stride + (x // 8)] |= 0x80 >> (x % 8)
    return bytes(out), stride


def unpack_icon(row_bytes, w, h, stride):
    """The exact inverse of pack_icon() - used only to prove the round trip
    in main(), never to build the header (which packs once and trusts it)."""
    bits = []
    for y in range(h):
        row = []
        for x in range(w):
            byte = row_bytes[y * stride + (x // 8)]
            row.append(bool(byte & (0x80 >> (x % 8))))
        bits.append(row)
    return bits


def count_runs(bits, w, h):
    """Total contiguous horizontal runs of "on" bits across every row - the
    same run-length walk icons.h's icon_bitmap_blocks() does at draw time,
    done here once at bake time so the cost of finding it is paid by the
    generator, not by every future caller."""
    total = 0
    for y in range(h):
        in_run = False
        for x in range(w):
            on = bits[y][x]
            if on and not in_run:
                total += 1
            in_run = on
    return total


# --- manifest ------------------------------------------------------------------

def load_manifest(path):
    try:
        manifest = json.loads(Path(path).read_text())
    except json.JSONDecodeError as exc:
        die("%s is not valid JSON: %s" % (path, exc))

    for key in ("cell_w", "cell_h", "icons"):
        if key not in manifest:
            die("%s: missing required key %r" % (path, key))
    cell_w, cell_h = manifest["cell_w"], manifest["cell_h"]
    if not (isinstance(cell_w, int) and cell_w > 0):
        die("%s: cell_w must be a positive integer" % path)
    if not (isinstance(cell_h, int) and cell_h > 0):
        die("%s: cell_h must be a positive integer" % path)
    if not isinstance(manifest["icons"], list):
        die("%s: 'icons' must be a list" % path)

    icons = []
    for i, entry in enumerate(manifest["icons"]):
        for key in ("name", "col", "row"):
            if key not in entry:
                die("%s: icons[%d] is missing %r" % (path, i, key))
        icons.append((entry["name"], entry["col"], entry["row"]))

    return cell_w, cell_h, icons


# --- emit ----------------------------------------------------------------------

def emit(w_stdout, prefix, cmd, source_png, cell_w, cell_h, baked):
    """`baked` is a list of (name, w, h, stride, offset, blocks, packed_bytes),
    already validated and packed by main()."""
    id_type = "icon_%s_id_t" % prefix
    count_name = "ICON_%s_COUNT" % prefix.upper()
    table_name = "icon_%s_table" % prefix
    rows_name = "icon_%s_rows" % prefix

    blob = bytearray()
    for _name, _w, _h, _stride, _offset, _blocks, packed in baked:
        blob += packed

    w = w_stdout
    w("/*=============================================================================\n")
    w(" * GENERATED FILE - do not edit.\n")
    w(" *\n")
    w(" *     %s\n" % cmd)
    w(" *\n")
    body = ("Baked from %s (%dx%d cells) - see gfx/icon.h for icon_t's own "
            "fields and tools/gen_icons.py for the PNG decode, validation "
            "and packing this table was produced by." %
            (source_png, cell_w, cell_h))
    for line in textwrap.wrap(body, width=75):
        w(" * %s\n" % line)
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n")
    w("#include <stdint.h>\n\n")
    w('#include "gfx/icon.h"\n\n')

    w("typedef enum {\n")
    for name, *_rest in baked:
        w("    ICON_%s_%s,\n" % (prefix.upper(), name.upper()))
    w("    %s\n" % count_name)
    w("} %s;\n\n" % id_type)

    w("static const uint8_t %s[%d] = {\n" % (rows_name, len(blob)))
    for i in range(0, len(blob), 20):
        chunk = blob[i:i + 20]
        w("    " + ", ".join("0x%02X" % b for b in chunk) + ",\n")
    w("};\n\n")

    w("static const icon_t %s[%s] = {\n" % (table_name, count_name))
    for name, iw, ih, stride, offset, blocks, _packed in baked:
        w("    [ICON_%s_%s] = { .offset = %d, .w = %d, .h = %d, "
          ".stride = %d, .blocks = %d },\n" %
          (prefix.upper(), name.upper(), offset, iw, ih, stride, blocks))
    w("};\n")


def main(argv):
    if len(argv) != 3:
        die("usage: gen_icons.py <atlas.png> <manifest.json> > <output.h>\n"
            "  the emitted prefix (icon_<prefix>_*) is the manifest's own "
            "filename stem, e.g. system.json -> icon_system_*")
    # argparse would be the usual choice, but this generator takes exactly
    # two required positionals - matching gen_boot_anim_image.py's plain
    # sys.argv handling rather than pulling in argparse for two values.
    png_path, json_path = argv[1], argv[2]

    prefix = Path(json_path).stem

    sys.stdout.reconfigure(newline="\n")

    cell_w, cell_h, manifest_icons = load_manifest(json_path)
    width, height, pixels = read_png(png_path)

    if width % cell_w != 0 or height % cell_h != 0:
        die("%s is %dx%d, which does not divide evenly into %dx%d cells "
            "(%s's own cell_w/cell_h)" %
            (png_path, width, height, cell_w, cell_h, json_path))
    cols, rows = width // cell_w, height // cell_h

    # Decode every pixel to a strict on/off bit up front - a single bad
    # pixel anywhere in the atlas must die() before any cell-level
    # validation runs, so the first error reported is always the root cause.
    bit_grid = [[pixel_to_bit(pixels[y][x], x, y, png_path)
                for x in range(width)] for y in range(height)]

    # --- validate names: unique, valid C identifiers --------------------
    seen_names = {}
    for name, col, row in manifest_icons:
        if not IDENTIFIER_RE.match(name):
            die("%s: icon name %r is not a valid C identifier" %
                (json_path, name))
        if name in seen_names:
            die("%s: icon name %r used twice (cells (%d,%d) and (%d,%d))" %
                (json_path, name, col, row, *seen_names[name]))
        seen_names[name] = (col, row)
        if not (0 <= col < cols and 0 <= row < rows):
            die("%s: icon %r at cell (%d,%d) is outside the %dx%d grid "
                "%s's dimensions imply" %
                (json_path, name, col, row, cols, rows, png_path))

    named_cells = {(col, row): name for name, (col, row) in seen_names.items()}

    def cell_bits(col, row):
        return [[bit_grid[row * cell_h + y][col * cell_w + x]
                for x in range(cell_w)] for y in range(cell_h)]

    def cell_is_empty(col, row):
        return not any(any(r) for r in cell_bits(col, row))

    # --- every named cell non-empty, every non-empty cell named ----------
    for name, (col, row) in seen_names.items():
        if cell_is_empty(col, row):
            die("%s: icon %r at cell (%d,%d) is empty - nothing was drawn "
                "there" % (json_path, name, col, row))
    for row in range(rows):
        for col in range(cols):
            if (col, row) not in named_cells and not cell_is_empty(col, row):
                die("%s: cell (%d,%d) in %s has artwork but no name in the "
                    "manifest - an unnamed drawing in the atlas is a "
                    "mistake, not a spare" % (json_path, col, row, png_path))

    # --- pack + round-trip + run-count, per icon --------------------------
    baked = []
    offset = 0
    for name, col, row in manifest_icons:
        bits = cell_bits(col, row)
        packed, stride = pack_icon(bits, cell_w, cell_h)

        roundtrip = unpack_icon(packed, cell_w, cell_h, stride)
        if roundtrip != bits:
            die("%s: icon %r failed its pack/unpack round trip - "
                "pack_icon()/unpack_icon() disagree about the bit layout, "
                "which means the baked bytes would not reproduce this "
                "artwork" % (json_path, name))

        blocks = count_runs(bits, cell_w, cell_h)
        if blocks > RUN_COUNT_CAP:
            die("%s: icon %r bakes to %d runs, over RUN_COUNT_CAP (%d) - "
                "ui_draw_bitmap()'s stack buffer (UI_DRAW_BITMAP_MAX_BLOCKS, "
                "ui.h) cannot hold this icon's shape; simplify the artwork "
                "or raise both caps together" % (json_path, name, blocks, RUN_COUNT_CAP))

        baked.append((name, cell_w, cell_h, stride, offset, blocks, packed))
        offset += len(packed)

    if offset > 0xFFFF:
        die("baked rows blob is %d bytes, too large for icon_t.offset "
            "(uint16_t)" % offset)

    cmd = ("python tools/gen_icons.py %s %s > main/gfx/icons_%s.h" %
           (png_path, json_path, prefix))
    emit(sys.stdout.write, prefix, cmd, png_path, cell_w, cell_h, baked)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
