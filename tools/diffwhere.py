#!/usr/bin/env python3
"""Where the interior differences in a ppmcmp diff map are, grouped.

ppmcmp answers how many pixels disagree and which class they are in; it cannot
answer the question that comes next, which is whether fifteen differing pixels
are one hairline crack along a shared edge, a sliver of a triangle that landed
in the wrong place, or fifteen scattered pixels of the wrong colour. Those are
three different faults with three different fixes, and telling them apart from
a total is guesswork.

The map is already written - ppmcmp -o paints interior differences pure red and
boundary ones blue over a dimmed copy of the capture - so this reads it back
and groups the red pixels into connected runs. Coordinates are relative to the
compared region, which is the movie's stage, so they can be put straight beside
the geometry in tests/swf-host/mkswf.c.

  tools/diffwhere.py out/dc/t_hole-diff.ppm [-n 12]

With --at it answers the question that always comes next - what colour is it -
against a capture rather than a diff map. Coordinates are the movie's own,
because that is what the geometry in tests/swf-host/mkswf.c is written in and
what a group listed above is reported in; the capture's "# region" comment says
where the stage landed on the console's 640x480 screen and this maps through
it. A row of them tells a stray fill apart from a faint one, which a count of
differing pixels cannot.

  tools/diffwhere.py out/dc/t_hole.ppm --at 41,98 --at 42,98 --at 45,98
  tools/diffwhere.py out/dc/t_hole.ppm --row 98,40,100     whole row, stage x
  tools/diffwhere.py out/dc/t_stroke.ppm --col 80,176,204  down through an edge

--col is the one for an edge that is soft when it should be hard: it walks
through the boundary rather than along it, so the profile of the transition is
the answer rather than something to be inferred from two samples. The screen
row is printed beside the stage row, because an artefact that sits at a fixed
screen position is the chrome or the display and an artefact that sits at a
fixed stage position is the movie.
"""
import sys

INTERIOR = (255, 0, 0)


def read_ppm(path):
    """Returns width, height, pixels and the stage rectangle if the header
    names one. popsurf writes "# region x y w h" into every capture, which is
    the only record of where layout put the movie."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise SystemExit(f"{path}: not a P6 PPM")
    fields, region, i = [], None, 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            j = data.index(b"\n", i)
            words = data[i + 1 : j].split()
            if len(words) == 5 and words[0] == b"region":
                region = tuple(int(v) for v in words[1:])
            i = j
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, _ = fields
    return w, h, data[i + 1 :], region


def clusters(w, h, px):
    """Connected groups of interior pixels, four-connectivity, iterative."""
    hit = bytearray(w * h)
    for k in range(w * h):
        if (px[k * 3], px[k * 3 + 1], px[k * 3 + 2]) == INTERIOR:
            hit[k] = 1

    out = []
    for k in range(w * h):
        if not hit[k]:
            continue
        stack, cells = [k], []
        hit[k] = 0
        while stack:
            c = stack.pop()
            cells.append(c)
            x, y = c % w, c // w
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if 0 <= nx < w and 0 <= ny < h and hit[ny * w + nx]:
                    hit[ny * w + nx] = 0
                    stack.append(ny * w + nx)
        xs = [c % w for c in cells]
        ys = [c // w for c in cells]
        out.append((len(cells), min(xs), min(ys), max(xs), max(ys)))
    out.sort(reverse=True)
    return out


def sample(path, w, h, px, region, pts):
    """Pixel values at stage coordinates, mapped through the capture's region."""
    ox, oy = (region[0], region[1]) if region else (0, 0)

    print(f"  {path}, stage origin {ox},{oy}:")
    for sx, sy in pts:
        x, y = sx + ox, sy + oy
        if not (0 <= x < w and 0 <= y < h):
            print(f"    stage {sx},{sy} is outside the picture")
            continue
        k = (y * w + x) * 3
        r, g, b = px[k], px[k + 1], px[k + 2]
        # Both channels: what the console holds, and what a 565 comparison
        # sees, since a difference that vanishes under quantisation is not a
        # difference ppmcmp ever counted.
        print(f"    stage {sx:4d},{sy:<4d} screen {x:4d},{y:<4d}  "
              f"{r:02x}{g:02x}{b:02x}   565 {r & 0xf8:02x}{g & 0xfc:02x}"
              f"{b & 0xf8:02x}")


def main(argv):
    path, limit, pts = None, 12, []
    i = 1
    while i < len(argv):
        if argv[i] == "-n" and i + 1 < len(argv):
            limit = int(argv[i + 1])
            i += 1
        elif argv[i] == "--at" and i + 1 < len(argv):
            sx, sy = argv[i + 1].split(",")
            pts.append((int(sx), int(sy)))
            i += 1
        elif argv[i] == "--row" and i + 1 < len(argv):
            sy, x0, x1 = (int(v) for v in argv[i + 1].split(","))
            pts.extend((x, sy) for x in range(x0, x1))
            i += 1
        elif argv[i] == "--col" and i + 1 < len(argv):
            sx, y0, y1 = (int(v) for v in argv[i + 1].split(","))
            pts.extend((sx, y) for y in range(y0, y1))
            i += 1
        else:
            path = argv[i]
        i += 1
    if not path:
        raise SystemExit(__doc__)

    w, h, px, region = read_ppm(path)

    if pts:
        sample(path, w, h, px, region, pts)
        return 0

    groups = clusters(w, h, px)
    if not groups:
        print("  no interior differences to place")
        return 0

    total = sum(g[0] for g in groups)
    print(f"  {total} interior pixels in {len(groups)} group(s), "
          f"stage coordinates:")
    for n, x0, y0, x1, y1 in groups[:limit]:
        shape = ("column" if x0 == x1 else
                 "row" if y0 == y1 else
                 f"{x1 - x0 + 1}x{y1 - y0 + 1}")
        print(f"    {n:6d} px  x {x0}..{x1}  y {y0}..{y1}  ({shape})")
    if len(groups) > limit:
        print(f"    ... and {len(groups) - limit} more")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
