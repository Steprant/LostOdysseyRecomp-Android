#!/usr/bin/env python3
"""Compare two sets of bit-exact frame samples (Steprant 2.0, WP0.1).

Each input is a Tester export ZIP (with frame-samples/), a frame-samples
directory, or a single PNG. Frames are matched by their index in frames.json
(or by sorted file name). For every image pair the script reports whether
it is bit-identical, how many pixels differ, the largest channel error, and
writes a PNG difference map (differences amplified, unchanged pixels dark).

  python3 tools/android/compare-frames.py before.zip after.zip [--out diffs/]
  python3 tools/android/compare-frames.py a.png b.png --out diff.png

Exit status 0 when every compared pair is identical, 1 otherwise, 2 on error.
Standard library only.
"""
import argparse
import io
import json
import struct
import sys
import zipfile
import zlib
from pathlib import Path


def decode_png(data):
    """Returns (width, height, rows of RGBA bytes). 8-bit RGB/RGBA/grey."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, color, idat = 8, 0, 0, 0, bytearray()
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace:
                raise ValueError("only 8-bit non-interlaced PNGs are supported")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 6: 4, 4: 2}.get(color)
    if not channels:
        raise ValueError(f"unsupported PNG colour type {color}")
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            upleft = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 255
            elif kind == 2:
                line[x] = (line[x] + up) & 255
            elif kind == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 255
            elif kind == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                line[x] = (line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else upleft)) & 255
        previous = line
        if channels == 4:
            rows.append(bytes(line))
        else:
            out = bytearray()
            for x in range(width):
                px = line[x * channels:(x + 1) * channels]
                if channels == 3:
                    out += px + b"\xff"
                elif channels == 1:
                    out += bytes([px[0]] * 3) + b"\xff"
                else:
                    out += bytes([px[0]] * 3) + bytes([px[1]])
            rows.append(bytes(out))
    return width, height, rows


def encode_png(width, height, rows):
    raw = b"".join(b"\x00" + row for row in rows)
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def fnv1a64(rows):
    h = 1469598103934665603
    for row in rows:
        for byte in row:
            h ^= byte
            h = (h * 1099511628211) & 0xffffffffffffffff
    return f"{h:016x}"


def compare(a, b):
    """Returns a dict of statistics and the difference map rows."""
    (wa, ha, ra), (wb, hb, rb) = a, b
    if (wa, ha) != (wb, hb):
        return {"identical": False, "size_mismatch": [[wa, ha], [wb, hb]]}, None
    differing, worst, diff_rows = 0, 0, []
    for row_a, row_b in zip(ra, rb):
        out = bytearray(len(row_a))
        if row_a == row_b:
            diff_rows.append(b"\x00\x00\x00\xff" * wa)
            continue
        for x in range(wa):
            i = x * 4
            delta = max(abs(row_a[i + c] - row_b[i + c]) for c in range(4))
            if delta:
                differing += 1
                worst = max(worst, delta)
                v = min(255, 64 + delta * 16)
                out[i:i + 4] = bytes([v, 0, 255 - v, 255])
            else:
                out[i:i + 4] = b"\x00\x00\x00\xff"
        diff_rows.append(bytes(out))
    return {"identical": differing == 0, "differing_pixels": differing, "pixels": wa * ha,
            "max_channel_error": worst}, diff_rows


class Source:
    """Images of one capture: name -> PNG bytes, plus frames.json if any."""

    def __init__(self, path):
        self.path, self.images, self.manifest = Path(path), {}, None
        if self.path.suffix.lower() == ".png":
            self.images[self.path.name] = self.path.read_bytes()
        elif self.path.is_dir():
            root = self.path / "frame-samples" if (self.path / "frame-samples").is_dir() else self.path
            for file in sorted(root.glob("*.png")):
                self.images[file.name] = file.read_bytes()
            if (root / "frames.json").is_file():
                self.manifest = json.loads((root / "frames.json").read_text(encoding="utf-8"))
        elif zipfile.is_zipfile(self.path):
            with zipfile.ZipFile(self.path) as archive:
                for name in sorted(archive.namelist()):
                    if name.startswith("frame-samples/") and name.endswith(".png"):
                        self.images[name.split("/", 1)[1]] = archive.read(name)
                    elif name == "frame-samples/frames.json":
                        self.manifest = json.loads(archive.read(name).decode("utf-8"))
        else:
            raise ValueError(f"{path}: not a PNG, a directory or a ZIP")
        if not self.images:
            raise ValueError(f"{path}: no frame samples found")

    def keyed(self):
        """kind ('presented'/'frontbuffer'/'image') + frame index -> name."""
        out = {}
        for position, name in enumerate(sorted(self.images)):
            parts = name[:-4].split("-")
            if len(parts) >= 4 and parts[0] == "frame":
                out[(parts[-1], int(parts[1]))] = name
            else:
                out[("image", position)] = name
        return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--out", help="directory (or .png for a single pair) for difference maps")
    parser.add_argument("--json", action="store_true", help="print one JSON report instead of text")
    args = parser.parse_args(argv)
    try:
        before, after = Source(args.before), Source(args.after)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"compare-frames: {error}", file=sys.stderr)
        return 2
    kb, ka = before.keyed(), after.keyed()
    pairs = sorted(set(kb) & set(ka)) if len(kb) > 1 or len(ka) > 1 else [(next(iter(kb)), next(iter(ka)))]
    report, all_identical = [], True
    out = Path(args.out) if args.out else None
    for pair in pairs:
        key_b, key_a = (pair, pair) if isinstance(pair[0], str) else pair
        name_b, name_a = kb[key_b], ka[key_a]
        try:
            img_b, img_a = decode_png(before.images[name_b]), decode_png(after.images[name_a])
        except (ValueError, zlib.error) as error:
            print(f"compare-frames: {name_b} / {name_a}: {error}", file=sys.stderr)
            return 2
        stats, diff = compare(img_b, img_a)
        stats.update({"before": name_b, "after": name_a,
                      "fnv1a64_before": fnv1a64(img_b[2]), "fnv1a64_after": fnv1a64(img_a[2])})
        if out and diff is not None and not stats["identical"]:
            target = out if out.suffix.lower() == ".png" and len(pairs) == 1 else out / f"diff-{name_a}"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(encode_png(img_a[0], img_a[1], diff))
            stats["diff_map"] = str(target)
        all_identical &= stats["identical"]
        report.append(stats)
    unmatched = sorted(set(kb) ^ set(ka)) if len(kb) > 1 or len(ka) > 1 else []
    if args.json:
        print(json.dumps({"identical": all_identical, "pairs": report,
                          "unmatched": [list(map(str, u)) for u in unmatched]}, indent=2))
    else:
        for stats in report:
            if stats["identical"]:
                print(f"{stats['after']}: identical ({stats['fnv1a64_after']})")
            elif "size_mismatch" in stats:
                print(f"{stats['after']}: size differs {stats['size_mismatch']}")
            else:
                share = 100.0 * stats["differing_pixels"] / max(1, stats["pixels"])
                print(f"{stats['after']}: {stats['differing_pixels']} pixels differ ({share:.4f}%), "
                      f"max channel error {stats['max_channel_error']}" +
                      (f", map {stats['diff_map']}" if "diff_map" in stats else ""))
        for u in unmatched:
            print(f"unmatched: {u[0]} frame {u[1]}")
        print("ALL IDENTICAL" if all_identical and report else "DIFFERENCES FOUND" if report else "NOTHING COMPARED")
    return 0 if all_identical and report else 1


if __name__ == "__main__":
    sys.exit(main())
