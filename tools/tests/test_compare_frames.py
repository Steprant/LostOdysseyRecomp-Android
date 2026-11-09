"""Host tests of the WP0.1 frame samples: the runtime's PNG writer and hash
(diagnostics/frame_samples.h) against tools/android/compare-frames.py."""
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("compare_frames", ROOT / "tools/android/compare-frames.py")
compare_frames = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare_frames)

WRITER = r'''
#include <diagnostics/frame_samples.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    namespace fs = diagnostics::frame_samples;
    const uint32_t w = 300, h = 230; // > 65535 bytes of scanlines: several stored blocks
    const int variant = std::atoi(argv[2]);
    std::vector<uint32_t> pixels(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            pixels[y * w + x] = 0xff000000u | ((x * 7) & 255) | (((y * 3) & 255) << 8) | (((x ^ y) & 255) << 16);
    if (variant == 1) pixels[17 * w + 33] ^= 0x00000005u; // one pixel, red off by 5
    const auto png = fs::EncodePng(pixels, w, h);
    FILE* f = std::fopen(argv[1], "wb");
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    std::printf("%016llx\n", static_cast<unsigned long long>(fs::Hash(pixels)));
    return 0;
}
'''


class CompareFramesTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def build_writer(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no C++ compiler")
        source = self.dir / "writer.cpp"
        source.write_text(WRITER, encoding="utf-8")
        binary = self.dir / "writer"
        subprocess.run([compiler, "-std=c++20", "-O1", "-I", str(ROOT / "LostOdysseyRecomp"), str(source), "-o", str(binary)],
                       check=True)
        return binary

    def test_runtime_png_round_trip_and_hash(self):
        writer = self.build_writer()
        first, second = self.dir / "a.png", self.dir / "b.png"
        hash_a = subprocess.run([str(writer), str(first), "0"], check=True, capture_output=True, text=True).stdout.strip()
        hash_b = subprocess.run([str(writer), str(second), "1"], check=True, capture_output=True, text=True).stdout.strip()
        width, height, rows = compare_frames.decode_png(first.read_bytes())
        self.assertEqual((width, height), (300, 230))
        self.assertEqual(rows[5][4 * 10:4 * 10 + 4], bytes([70, 15, 10 ^ 5, 255]))
        self.assertEqual(compare_frames.fnv1a64(rows), hash_a, "runtime hash = script hash of the decoded PNG")
        self.assertNotEqual(hash_a, hash_b)
        # Identical pair, then one pixel off by 5 with a difference map.
        self.assertEqual(compare_frames.main([str(first), str(first)]), 0)
        diff = self.dir / "diff.png"
        self.assertEqual(compare_frames.main([str(first), str(second), "--out", str(diff)]), 1)
        stats, _ = compare_frames.compare(compare_frames.decode_png(first.read_bytes()),
                                          compare_frames.decode_png(second.read_bytes()))
        self.assertEqual(stats["differing_pixels"], 1)
        self.assertEqual(stats["max_channel_error"], 5)
        self.assertTrue(diff.is_file())
        self.assertEqual(compare_frames.decode_png(diff.read_bytes())[:2], (300, 230))

    def test_tester_zip_capture_sets(self):
        writer = self.build_writer()
        for name, variant in (("before", 0), ("after", 1)):
            folder = self.dir / name
            folder.mkdir()
            subprocess.run([str(writer), str(folder / "frame-01-swap10-presented.png"), "0"], check=True, capture_output=True)
            subprocess.run([str(writer), str(folder / "frame-01-swap10-frontbuffer.png"), str(variant)], check=True,
                           capture_output=True)
            (folder / "frames.json").write_text(json.dumps({"frames": []}), encoding="utf-8")
            with zipfile.ZipFile(self.dir / f"{name}.zip", "w") as archive:
                for file in folder.iterdir():
                    archive.write(file, f"frame-samples/{file.name}")
                archive.writestr("runtime.log", "")
        out = self.dir / "maps"
        self.assertEqual(compare_frames.main([str(self.dir / "before.zip"), str(self.dir / "after.zip"),
                                              "--out", str(out)]), 1)
        self.assertTrue((out / "diff-frame-01-swap10-frontbuffer.png").is_file())
        self.assertFalse((out / "diff-frame-01-swap10-presented.png").exists(), "identical pairs write no map")
        self.assertEqual(compare_frames.main([str(self.dir / "before"), str(self.dir / "before.zip")]), 0)


if __name__ == "__main__":
    unittest.main()
