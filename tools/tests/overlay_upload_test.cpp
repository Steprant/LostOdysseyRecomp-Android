// Steprant 2.0.2: the Settings overlay copy never reads its destination and
// produces the bytes the old in-place masking produced; an unchanged image is
// not uploaded again (gpu/overlay_upload.h).
#include <gpu/overlay_upload.h>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>
using namespace gpu::overlay_upload;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
// The 2.0 loop: copy the row, then clear alpha in place.
static void Reference(uint32_t* row, const uint32_t* source, uint32_t width, uint32_t y, uint32_t header, uint32_t bottom,
    uint32_t left, uint32_t right)
{
    std::memcpy(row, source, size_t(width) * 4);
    if (y < header || y >= bottom)
        for (uint32_t x = 0; x < width; ++x) row[x] &= 0x00FFFFFFu;
    else {
        for (uint32_t x = 0; x < left; ++x) row[x] &= 0x00FFFFFFu;
        for (uint32_t x = right; x < width; ++x) row[x] &= 0x00FFFFFFu;
    }
}
int main()
{
    try {
        std::mt19937 rng(20261111);
        // The phone (2670x1200, 16:9 layout centred) and the TV, then random windows.
        struct Case { uint32_t width, height; Window window; };
        std::vector<Case> cases{
            {2670, 1200, {167, 1200, 268, 2402}},
            {1920, 1080, {150, 1080, 0, 1920}},
        };
        for (int i = 0; i < 300; ++i) {
            Case c;
            c.width = 1 + rng() % 300; c.height = 1 + rng() % 40;
            c.window.headerBottom = rng() % (c.height + 2);
            c.window.bottom = rng() % (c.height + 2);
            c.window.left = rng() % (c.width + 2);
            c.window.right = c.window.left + rng() % (c.width + 2 - c.window.left);
            cases.push_back(c);
        }
        bool same = true, untouched = true;
        for (const auto& c : cases) {
            std::vector<uint32_t> source(size_t(c.width) * c.height);
            for (auto& pixel : source) pixel = rng();
            // Guard words around the destination row: nothing outside it is written.
            std::vector<uint32_t> expected(c.width), actual(c.width + 2);
            for (uint32_t y = 0; y < c.height; ++y) {
                const uint32_t left = std::min(c.window.left, c.width), right = std::min(std::max(c.window.right, left), c.width);
                Reference(expected.data(), &source[size_t(y) * c.width], c.width, y, c.window.headerBottom, c.window.bottom, left, right);
                actual.front() = 0xDEADBEEFu; actual.back() = 0xCAFEF00Du;
                // A destination full of garbage: a copy that read it would differ.
                for (uint32_t x = 0; x < c.width; ++x) actual[1 + x] = ~source[size_t(y) * c.width + x] ^ 0x5A5A5A5Au;
                CopyMaskedRow(actual.data() + 1, &source[size_t(y) * c.width], c.width, y, c.window);
                same &= std::memcmp(expected.data(), actual.data() + 1, size_t(c.width) * 4) == 0;
                untouched &= actual.front() == 0xDEADBEEFu && actual.back() == 0xCAFEF00Du;
            }
        }
        Check(same, "every row equals the copy-then-mask of 2.0, whatever the destination held");
        Check(untouched, "nothing is written outside the row");

        UploadState state;
        Check(state.NeedsUpload(true, 2670, 1200, 42), "the first image is uploaded");
        state.Uploaded(2670, 1200, 42);
        Check(!state.NeedsUpload(true, 2670, 1200, 42), "the same image is not uploaded again");
        state.Reused();
        Check(state.NeedsUpload(true, 2670, 1200, 43), "a changed image is (the cursor's sway, a fade)");
        Check(state.NeedsUpload(true, 1920, 1080, 42), "another size is");
        Check(state.NeedsUpload(false, 2670, 1200, 42), "LO_MENU_OVERLAY_REUSE=0 uploads every frame");
        state.Invalidate();
        Check(state.NeedsUpload(true, 2670, 1200, 42), "a new texture is uploaded whatever the hash");
        Check(state.uploads == 1 && state.reuses == 1, "uploads and reuses are counted");
        std::printf("overlay upload: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("overlay upload test failed: %s\n", e.what());
        return 1;
    }
}
