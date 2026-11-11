#pragma once
// Steprant 2.0.2: the Settings menu over a game frame without reading the
// GPU upload buffer, and without uploading an image that did not change.
//
// DrawSettingsOverlay copied the menu image (12.8 MB at 2670x1200) into the
// mapped upload buffer and then cleared the alpha of the header rows and the
// side bands in place: `row[x] &= 0x00FFFFFF` on mapped GPU memory, a read of
// uncached memory for every one of those pixels, every frame the menu was
// open. That is the "settings-overlay" phase of about 115 ms a frame in the
// 11/11 logs. Two changes:
//   - CopyMaskedRow writes each row once from the menu's own pixels and never
//     reads the destination;
//   - the image is uploaded only when it differs from the one uploaded last
//     (UploadState compares a hash of the pixels, so the menu's motion, which
//     does not change its revision, still uploads); otherwise the texture of
//     the previous frame is drawn again.
// LO_MENU_OVERLAY_REUSE=0 uploads every frame as before (still without the
// reads).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gpu::overlay_upload {

// Where the menu keeps its alpha: rows [headerBottom, bottom), columns [left, right).
struct Window
{
    uint32_t headerBottom = 0, bottom = 0, left = 0, right = 0;
};

// One row of `width` pixels from `source` to `destination` (which may be
// write-combined memory: it is only written, front to back). Outside the
// window the alpha is cleared.
inline void CopyMaskedRow(uint32_t* destination, const uint32_t* source, uint32_t width, uint32_t y, const Window& window)
{
    if (y < window.headerBottom || y >= window.bottom) {
        for (uint32_t x = 0; x < width; ++x) destination[x] = source[x] & 0x00FFFFFFu;
        return;
    }
    const uint32_t left = window.left < width ? window.left : width;
    const uint32_t right = window.right < left ? left : window.right < width ? window.right : width;
    for (uint32_t x = 0; x < left; ++x) destination[x] = source[x] & 0x00FFFFFFu;
    if (right > left) std::memcpy(destination + left, source + left, size_t(right - left) * 4);
    for (uint32_t x = right; x < width; ++x) destination[x] = source[x] & 0x00FFFFFFu;
}

// What the overlay texture holds.
struct UploadState
{
    bool valid = false;
    uint32_t width = 0, height = 0;
    uint64_t hash = 0;
    uint64_t uploads = 0, reuses = 0;

    // Whether the image (its size and the hash of its pixels) must be uploaded.
    bool NeedsUpload(bool reuseEnabled, uint32_t imageWidth, uint32_t imageHeight, uint64_t imageHash) const
    {
        return !reuseEnabled || !valid || width != imageWidth || height != imageHeight || hash != imageHash;
    }
    void Uploaded(uint32_t imageWidth, uint32_t imageHeight, uint64_t imageHash)
    {
        valid = true; width = imageWidth; height = imageHeight; hash = imageHash; ++uploads;
    }
    void Reused() { ++reuses; }
    void Invalidate() { valid = false; }
};

}
