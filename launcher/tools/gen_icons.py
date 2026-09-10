#!/usr/bin/env python3
"""Generate an icons_<name>.h from a manifest of PNG-atlas cells and/or SVG
files, each icon declaring its own source.

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

SVG: GRID EXTRACTION, NOT RASTERIZATION

pixelarticons (https://github.com/halfmage/pixelarticons, MIT) exports each
icon as a single <path> on an integer 24x24 grid made of nothing but
M/H/V/h/v/Z - every subpath is an axis-aligned rectangle. That means reading
one is exact: walk the path data, trace each subpath's corners, and fill the
pixel grid the rectangles describe - no rasterizer, no sampling, no
threshold. `read_svg_icon()` below is exactly as strict about this as
`read_png()` is about antialiasing: a curve command, a non-integer
coordinate, a subpath that does not close into an axis-aligned rectangle, or
a <path> count other than 1 is a rejection, never an approximation, because
the entire reason this source is safe is that it never needs approximating.

THE MANIFEST: ONE SOURCE PER ICON

A JSON object naming the PNG atlas's cell geometry, plus one entry per icon -
each entry declares its OWN "source", "png" or "svg", because a set can mix
hand-drawn atlas cells with imported SVGs and that provenance is a fact about
the icon, not the file:

    {
        "cell_w": 16, "cell_h": 16,
        "icons": [
            { "name": "check", "source": "png", "col": 0, "row": 0 },
            { "name": "close", "source": "svg", "file": "close.svg",
              "upstream": "close", "commit": "<pinned pixelarticons sha>" }
        ]
    }

A "svg" entry's "file" is looked up in <manifest-dir>/<manifest-stem>/ (e.g.
design/icons/system/close.svg for system.json) - checked-in files, never
fetched by this script. "upstream" and "commit" are required for every "svg"
entry: the icon this tree ships is only as trustworthy as knowing exactly
which upstream file, at which commit, it came from - see design/icons/
LICENSE-pixelarticons and docs/plans/Icon-Baker-Plan.md's "Provenance is a
first-class requirement" on why (the defaulticon set was rejected when its
own upstream vanished and its licence became unverifiable).

The PNG's own width/height must be an exact multiple of cell_w/cell_h - the
grid's row/column count is derived from that division, not carried
separately in the manifest, so the two files cannot disagree about it. Only
"png" entries live in that grid; "svg" entries carry their own size from
their <svg viewBox>, which is why icon_t's w/h/stride are per-icon fields
and nothing in this generator assumes every baked icon shares one size.

WHAT GETS REJECTED BEFORE ANYTHING IS EMITTED

Following gen_boot_anim_image.py/gen_font.py's own convention: every check
below runs to completion, and the header is only written once none of them
have called die() - see this file's own main() for the full order. Checked:
every PNG pixel strictly on or off; the PNG's dimensions divide evenly into
whole cells; every named PNG cell non-empty; every non-empty PNG cell named;
names unique across the whole manifest and valid C identifiers; every "svg"
entry has exactly one <path>, an integer viewBox, and path data using only
M/H/V/Z (any case) at integer coordinates whose subpaths are all closed
axis-aligned rectangles; each icon's run count at or under RUN_COUNT_CAP;
and, per icon, that packing its pixels into rows[] and unpacking them again
reproduces the exact same on/off grid that was decoded - the honest
self-check for a bit-packing routine, proving the ROUND TRIP rather than
re-deriving the same bits with different code and comparing (see
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

# icon_t.blocks (gfx/icon.h) is a uint8_t - the real remaining bound now
# that ui_draw_icon() (ui.c) streams runs instead of collecting them into a
# stack buffer. A count above 255 would silently wrap that field rather
# than fail loudly, so this stays a hard rejection, not a raise-when-
# convenient number.
#
# This is NOT the only budget: microui's command list (MU_COMMANDLIST_SIZE,
# 8 KiB) is a separate, still-live ceiling a run count does not lift - a
# 46-run icon alone is roughly 1.3 KiB of it. Detailed artwork is now
# possible; it is not free.
RUN_COUNT_CAP = 255

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


# --- SVG decoding (stdlib only) ----------------------------------------------

# Every character a pixelarticons path is allowed to contain. Anything else
# (a curve command, a comma-less exponent marker, stray punctuation) fails
# this before the tokenizer ever runs, so an unsupported command is always
# named by the exact character that triggered it.
_D_ALLOWED_CHARS = set("MmHhVvZz0123456789.,+- \t\r\n")

# Matches one command letter or one number (integer, decimal, or either with
# an exponent) - deliberately permissive about SHAPE so a non-integer number
# is tokenized whole and rejected with its own text, rather than splitting on
# the '.' and reporting a confusing half-token.
_D_TOKEN_RE = re.compile(r"[A-Za-z]|-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?")


def _svg_path_rects(d, path):
    """Path data (the <path d="..."> string) -> list of (x0, y0, x1, y1)
    rectangles, each meaning "fill columns [x0,x1) rows [y0,y1)". Walks each
    M...Z subpath's corners and requires the result to close into exactly
    one axis-aligned rectangle - see this file's top comment on why SVG
    reading is grid extraction, not rasterization, for pixelarticons' shape
    of source."""
    for ch in d:
        if ch not in _D_ALLOWED_CHARS:
            die("%s: path data contains %r, which is not one of M/H/V/Z "
                "(any case) or an integer coordinate - curves and other "
                "commands are not supported" % (path, ch))

    tokens = _D_TOKEN_RE.findall(d)
    n = len(tokens)
    i = 0

    def next_number(ctx):
        nonlocal i
        if i >= n or tokens[i][:1].isalpha():
            die("%s: path data: %s expects a coordinate" % (path, ctx))
        tok = tokens[i]
        if "." in tok or "e" in tok or "E" in tok:
            die("%s: path data contains a non-integer coordinate %r - only "
                "integer coordinates are supported" % (path, tok))
        i += 1
        return int(tok)

    rects = []
    cx = cy = 0
    while i < n:
        cmd = tokens[i]
        if cmd[:1].isalpha() and cmd not in ("M", "m"):
            die("%s: path data has %r outside a subpath (subpaths must "
                "start with M/m)" % (path, cmd))
        if not cmd[:1].isalpha():
            die("%s: path data has coordinate %r with no command letter "
                "before it - implicit repeated commands are not supported" %
                (path, cmd))
        i += 1
        x, y = next_number("M/m"), next_number("M/m")
        # cx/cy still hold the previous subpath's start (Z always resets the
        # pen there) - correct for a relative "m" even on the very first
        # subpath, since cx/cy start at (0,0), SVG's own rule for an m with
        # no current point yet.
        cx, cy = (cx + x, cy + y) if cmd == "m" else (x, y)
        sx, sy = cx, cy
        corners = [(cx, cy)]

        while True:
            if i >= n:
                die("%s: subpath starting at (%d,%d) is never closed with "
                    "Z" % (path, sx, sy))
            nxt = tokens[i]
            if nxt in ("Z", "z"):
                i += 1
                break
            if nxt in ("M", "m"):
                die("%s: subpath starting at (%d,%d) is never closed with "
                    "Z before the next subpath begins" % (path, sx, sy))
            if nxt not in ("H", "h", "V", "v"):
                die("%s: path data uses unsupported command %r - only "
                    "M/H/V/Z (any case) are supported" % (path, nxt))
            i += 1
            val = next_number(nxt)
            if nxt in ("H", "h"):
                cx = val if nxt == "H" else cx + val
            else:
                cy = val if nxt == "V" else cy + val
            corners.append((cx, cy))

        cx, cy = sx, sy  # Z always resets the pen to the subpath's start

        # A subpath drawn as M + 3 H/V ends with Z supplying the 4th,
        # implicit edge; M + 4 H/V already returns to the start itself and Z
        # is a no-op close - both shapes appear in real pixelarticons files,
        # so a trailing corner equal to the start is a duplicate to drop,
        # not a 5th corner.
        if len(corners) > 1 and corners[-1] == corners[0]:
            corners = corners[:-1]

        if len(corners) != 4:
            die("%s: subpath starting at (%d,%d) has %d corners after "
                "closing (%r) - not the 4 an axis-aligned rectangle needs" %
                (path, sx, sy, len(corners), corners))

        xs = sorted(set(p[0] for p in corners))
        ys = sorted(set(p[1] for p in corners))
        if len(xs) != 2 or len(ys) != 2:
            die("%s: subpath starting at (%d,%d) is degenerate (corners "
                "%r) - not a rectangle with non-zero width and height" %
                (path, sx, sy, corners))

        edge_axes = []
        for k in range(4):
            p0, p1 = corners[k], corners[(k + 1) % 4]
            if p0[0] == p1[0] and p0[1] != p1[1]:
                edge_axes.append("V")
            elif p0[1] == p1[1] and p0[0] != p1[0]:
                edge_axes.append("H")
            else:
                die("%s: subpath starting at (%d,%d) has a non-axis-aligned "
                    "edge %r-%r" % (path, sx, sy, p0, p1))
        if any(edge_axes[k] == edge_axes[(k + 1) % 4] for k in range(4)):
            die("%s: subpath starting at (%d,%d) is not a rectangle - its "
                "edges (%r) do not alternate horizontal/vertical" %
                (path, sx, sy, edge_axes))

        rects.append((xs[0], ys[0], xs[1], ys[1]))

    if not rects:
        die("%s: path data has no subpaths" % path)
    return rects


def read_svg_icon(path):
    """A pixelarticons-shaped SVG -> (width, height, bits), bits[y][x] a
    bool, exactly like read_png()'s pixel grid but built from traced
    rectangles instead of decoded pixels. Rejects anything but exactly one
    <path> and an integer viewBox - see this file's top comment."""
    text = Path(path).read_text()

    path_tags = re.findall(r"<path\b[^>]*/?>", text)
    if len(path_tags) != 1:
        die("%s: expected exactly one <path> element, found %d" %
            (path, len(path_tags)))
    d_match = re.search(r'\bd\s*=\s*"([^"]*)"', path_tags[0])
    if not d_match:
        die("%s: the <path> element has no d attribute" % path)

    vb_match = re.search(r'\bviewBox\s*=\s*"([^"]*)"', text)
    if not vb_match:
        die("%s: <svg> has no viewBox attribute" % path)
    vb_raw = vb_match.group(1)
    vb_parts = vb_raw.replace(",", " ").split()
    if len(vb_parts) != 4:
        die("%s: viewBox %r is not four numbers" % (path, vb_raw))
    try:
        min_x, min_y, width, height = (int(p) for p in vb_parts)
    except ValueError:
        die("%s: viewBox %r is not four integers" % (path, vb_raw))
    if width <= 0 or height <= 0:
        die("%s: viewBox %r has a non-positive width or height" %
            (path, vb_raw))
    if width > 255 or height > 255:
        die("%s: viewBox %r is %dx%d, too large for icon_t's uint8_t w/h" %
            (path, vb_raw, width, height))

    rects = _svg_path_rects(d_match.group(1), path)

    bits = [[False] * width for _ in range(height)]
    for x0, y0, x1, y1 in rects:
        rx0, ry0, rx1, ry1 = x0 - min_x, y0 - min_y, x1 - min_x, y1 - min_y
        if rx0 < 0 or ry0 < 0 or rx1 > width or ry1 > height:
            die("%s: a rectangle at (%d,%d)-(%d,%d) falls outside the "
                "%dx%d viewBox" % (path, x0, y0, x1, y1, width, height))
        for y in range(ry0, ry1):
            for x in range(rx0, rx1):
                bits[y][x] = True

    if not any(any(row) for row in bits):
        die("%s: decodes to an empty grid - nothing was drawn" % path)

    return width, height, bits


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


def output_path_for(json_path):
    """Where this bake's own banner tells a future reader to redirect stdout.
    Not discovered - the script never sees its own `>` redirect - but derived
    from the manifest's location, matching docs/plans/Icon-Baker-Plan.md's
    ownership split: design/icons/<x>.json bakes to the shared gfx/ atlas, an
    app's own apps/<name>/icons/<x>.json bakes beside that app's folder."""
    manifest_dir = Path(json_path).parent
    prefix = Path(json_path).stem
    if manifest_dir.name == "icons" and manifest_dir.parent.name != "design":
        return "%s/icons_%s.h" % (manifest_dir.parent.as_posix(), prefix)
    return "main/gfx/icons_%s.h" % prefix


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
        if "name" not in entry:
            die("%s: icons[%d] is missing 'name'" % (path, i))
        source = entry.get("source")
        if source == "png":
            for key in ("col", "row"):
                if key not in entry:
                    die("%s: icons[%d] (%r, source \"png\") is missing %r" %
                        (path, i, entry["name"], key))
        elif source == "svg":
            for key in ("file", "upstream", "commit"):
                if key not in entry:
                    die("%s: icons[%d] (%r, source \"svg\") is missing %r - "
                        "an imported icon must record its upstream name and "
                        "the pinned commit it came from" %
                        (path, i, entry["name"], key))
        else:
            die("%s: icons[%d] (%r) has 'source' %r - must be \"png\" or "
                "\"svg\"" % (path, i, entry.get("name"), source))
        icons.append(entry)

    return cell_w, cell_h, icons


# --- emit ----------------------------------------------------------------------

def emit(w_stdout, prefix, cmd, source_png, cell_w, cell_h, svg_commits, baked):
    """`baked` is a list of (name, w, h, stride, offset, blocks, packed_bytes),
    already validated and packed by main(). `svg_commits` is the sorted set
    of pixelarticons commits any "svg" icon in this bake was pinned to,
    empty if the manifest names none."""
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
            "fields and tools/gen_icons.py for the PNG/SVG decode, "
            "validation and packing this table was produced by." %
            (source_png, cell_w, cell_h))
    for line in textwrap.wrap(body, width=75):
        w(" * %s\n" % line)
    if svg_commits:
        w(" *\n")
        body = ("Imported icons are pixelarticons (MIT, Gerrit Halfmann) - "
                 "see design/icons/LICENSE-pixelarticons - pinned to commit%s "
                 "%s." % ("s" if len(svg_commits) > 1 else "",
                          ", ".join(svg_commits)))
        for line in textwrap.wrap(body, width=75):
            w(" * %s\n" % line)
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n")
    w("#include <stdint.h>\n\n")
    w('#include "gfx/icon.h"\n\n')

    w("typedef enum {\n")
    for entry, *_rest in baked:
        w("    ICON_%s_%s,\n" % (prefix.upper(), entry.upper()))
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

    # What this atlas would cost a caller that drew every icon once. A
    # screen drawing a known set can size its own share of the command
    # list against this rather than counting rects by hand.
    total_blocks = sum(b for _n, _w2, _h2, _s, _o, b, _p in baked)
    w("\n/* Rects every icon here emits if all are drawn once - a\n"
      " * command-list cost, not just a count. */\n")
    w("#define ICON_%s_TOTAL_BLOCKS %d\n" % (prefix.upper(), total_blocks))

    # Facts the CONSUMER's own compile can check. The table and the blob it
    # indexes are emitted together and can only disagree through a generator
    # bug - which would otherwise surface as a wrong glyph at draw time, far
    # from its cause.
    w("\n/* Pins this table against its own blob, so a bad offset or\n"
      " * stride is a compile error where the header is included rather\n"
      " * than a wrong glyph at draw time. */\n")
    w("_Static_assert(sizeof %s == %d,\n"
      "               \"%s was rebaked without its offsets\");\n"
      % (rows_name, len(blob), rows_name))
    for name, iw, ih, stride, offset, _blocks, _packed in baked:
        w("_Static_assert(%d + %d * %d <= (int)sizeof %s,\n"
          "               \"icon %s runs past the end of %s\");\n"
          % (offset, ih, stride, rows_name, name, rows_name))
        w("_Static_assert(%d == (%d + 7) / 8,\n"
          "               \"icon %s stride does not match its width\");\n"
          % (stride, iw, name))


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
    svg_dir = Path(json_path).parent / prefix

    sys.stdout.reconfigure(newline="\n")

    cell_w, cell_h, manifest_icons = load_manifest(json_path)
    width, height, pixels = read_png(png_path)

    if width % cell_w != 0 or height % cell_h != 0:
        die("%s is %dx%d, which does not divide evenly into %dx%d cells "
            "(%s's own cell_w/cell_h)" %
            (png_path, width, height, cell_w, cell_h, json_path))
    cols, rows = width // cell_w, height // cell_h

    # Decode every PNG pixel to a strict on/off bit up front - a single bad
    # pixel anywhere in the atlas must die() before any cell-level
    # validation runs, so the first error reported is always the root cause.
    bit_grid = [[pixel_to_bit(pixels[y][x], x, y, png_path)
                for x in range(width)] for y in range(height)]

    # --- validate names: unique across the WHOLE manifest, valid C ids ---
    seen_names = set()
    png_entries = []
    for entry in manifest_icons:
        name = entry["name"]
        if not IDENTIFIER_RE.match(name):
            die("%s: icon name %r is not a valid C identifier" %
                (json_path, name))
        if name in seen_names:
            die("%s: icon name %r used twice" % (json_path, name))
        seen_names.add(name)
        if entry["source"] == "png":
            col, row = entry["col"], entry["row"]
            if not (0 <= col < cols and 0 <= row < rows):
                die("%s: icon %r at cell (%d,%d) is outside the %dx%d grid "
                    "%s's dimensions imply" %
                    (json_path, name, col, row, cols, rows, png_path))
            png_entries.append(entry)

    named_cells = {(e["col"], e["row"]): e["name"] for e in png_entries}

    def cell_bits(col, row):
        return [[bit_grid[row * cell_h + y][col * cell_w + x]
                for x in range(cell_w)] for y in range(cell_h)]

    def cell_is_empty(col, row):
        return not any(any(r) for r in cell_bits(col, row))

    # --- every named PNG cell non-empty, every non-empty PNG cell named ---
    for entry in png_entries:
        col, row = entry["col"], entry["row"]
        if cell_is_empty(col, row):
            die("%s: icon %r at cell (%d,%d) is empty - nothing was drawn "
                "there" % (json_path, entry["name"], col, row))
    for row in range(rows):
        for col in range(cols):
            if (col, row) not in named_cells and not cell_is_empty(col, row):
                die("%s: cell (%d,%d) in %s has artwork but no name in the "
                    "manifest - an unnamed drawing in the atlas is a "
                    "mistake, not a spare" % (json_path, col, row, png_path))

    # --- extract each icon's own (w, h, bits), pack, round-trip, count ----
    baked = []
    svg_commits = set()
    offset = 0
    for entry in manifest_icons:
        name = entry["name"]
        if entry["source"] == "png":
            iw, ih = cell_w, cell_h
            bits = cell_bits(entry["col"], entry["row"])
        else:
            svg_path = svg_dir / entry["file"]
            if not svg_path.is_file():
                die("%s: icon %r names svg file %s, which does not exist" %
                    (json_path, name, svg_path))
            iw, ih, bits = read_svg_icon(str(svg_path))
            svg_commits.add(entry["commit"])

        packed, stride = pack_icon(bits, iw, ih)

        roundtrip = unpack_icon(packed, iw, ih, stride)
        if roundtrip != bits:
            die("%s: icon %r failed its pack/unpack round trip - "
                "pack_icon()/unpack_icon() disagree about the bit layout, "
                "which means the baked bytes would not reproduce this "
                "artwork" % (json_path, name))

        blocks = count_runs(bits, iw, ih)
        if blocks > RUN_COUNT_CAP:
            die("%s: icon %r bakes to %d runs, over RUN_COUNT_CAP (%d) - "
                "icon_t.blocks (gfx/icon.h) is a uint8_t and would silently "
                "wrap; simplify the artwork" % (json_path, name, blocks, RUN_COUNT_CAP))

        baked.append((name, iw, ih, stride, offset, blocks, packed))
        offset += len(packed)

    if offset > 0xFFFF:
        die("baked rows blob is %d bytes, too large for icon_t.offset "
            "(uint16_t)" % offset)

    cmd = ("python tools/gen_icons.py %s %s > %s" %
           (png_path, json_path, output_path_for(json_path)))
    emit(sys.stdout.write, prefix, cmd, png_path, cell_w, cell_h,
         sorted(svg_commits), baked)

    print("gen_icons.py: baked %d icon(s), run counts: %s" %
          (len(baked), ", ".join("%s=%d" % (b[0], b[5]) for b in baked)),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
