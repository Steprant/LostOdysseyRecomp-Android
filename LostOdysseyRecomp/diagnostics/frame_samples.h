#pragma once
// Bit-exact frame samples (Steprant 2.0, WP0.1). The developer page of the
// 1.9.x test builds asks for N consecutive presented frames; for each one the
// presentation path stores, losslessly, the image it presented and the game's
// resolved frontbuffer (the scene after the upscaler, before presentation
// scaling and the interface overlays of the host), plus a 64-bit hash of
// each. Files go to android-diagnostics/frame-samples/ (beside the Tester
// sessions); the Tester export puts them in its ZIP under frame-samples/.
// tools/android/compare-frames.py compares two captures pixel by pixel.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace diagnostics::frame_samples
{
// ---- Lossless PNG (stored deflate blocks; the ZIP export compresses) ------
inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// pixels: 0xAABBGGRR (R in the low byte), as present_capture and the renderer
// readback return them. Writes RGBA8, 8 bits per channel, no filtering.
inline std::vector<uint8_t> EncodePng(const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> out;
    if (!width || !height || pixels.size() != size_t(width) * height) return out;
    const auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
    };
    const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        put32(out, uint32_t(data.size()));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        put32(out, Crc32(out.data() + start, out.size() - start));
    };
    static constexpr uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), signature, signature + 8);
    std::vector<uint8_t> header;
    put32(header, width); put32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, no filter, no interlace
    chunk("IHDR", header);
    // Raw scanlines: filter byte 0 then RGBA.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t p = pixels[size_t(y) * width + x];
            raw.push_back(uint8_t(p)); raw.push_back(uint8_t(p >> 8)); raw.push_back(uint8_t(p >> 16)); raw.push_back(uint8_t(p >> 24));
        }
    }
    // zlib stream of stored blocks (<= 65535 bytes each) and Adler-32.
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (const uint8_t byte : raw) { a = (a + byte) % 65521u; b = (b + a) % 65521u; }
    size_t offset = 0;
    do {
        const size_t size = std::min<size_t>(65535, raw.size() - offset);
        const bool last = offset + size == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(size)); z.push_back(uint8_t(size >> 8));
        z.push_back(uint8_t(~size)); z.push_back(uint8_t(~size >> 8));
        z.insert(z.end(), raw.begin() + ptrdiff_t(offset), raw.begin() + ptrdiff_t(offset + size));
        offset += size;
    } while (offset < raw.size());
    put32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

// FNV-1a 64 over the RGBA bytes: stable across builds and platforms, and
// what compare-frames.py recomputes.
inline uint64_t Hash(const std::vector<uint32_t>& pixels)
{
    uint64_t h = 1469598103934665603ull;
    for (const uint32_t p : pixels)
        for (int i = 0; i < 4; ++i) { h ^= uint8_t(p >> (8 * i)); h *= 1099511628211ull; }
    return h;
}

// ---- Request state --------------------------------------------------------
struct State {
    std::mutex mutex;
    std::filesystem::path directory; // set at startup (android-diagnostics/frame-samples)
    uint32_t requested = 0, remaining = 0, captured = 0;
    uint32_t firstSwap = 0;
    std::string json;                // frames.json body being built
    std::vector<uint32_t> frontbuffer;
    uint32_t frontWidth = 0, frontHeight = 0, frontAddress = 0;
    bool frontAvailable = false;
    // SR4 M-3: the upscaler's color input of the same frame (scene before
    // SGSR2), when an upscaler ran; 10-bit and float inputs are stored as 8 bits.
    std::vector<uint32_t> scene;
    uint32_t sceneWidth = 0, sceneHeight = 0;
    bool sceneAvailable = false;
    uint32_t activeSwap = 0;
    std::string planText;            // render/output size and upscaler at the sample
};
inline State& Get() { static State state; return state; }
inline std::atomic<bool> pending{false};

inline void SetDirectory(const std::filesystem::path& directory)
{
    std::lock_guard lock(Get().mutex);
    Get().directory = directory;
}

// Asks for `count` consecutive frames from the next present. Clears the
// previous capture. False when one is already running or no directory.
inline bool Request(uint32_t count)
{
    auto& s = Get();
    std::lock_guard lock(s.mutex);
    if (s.remaining || s.directory.empty() || !count) return false;
    std::error_code ec;
    std::filesystem::remove_all(s.directory, ec);
    std::filesystem::create_directories(s.directory, ec);
    if (ec) return false;
    s.requested = s.remaining = std::min(count, 8u);
    s.captured = 0;
    s.firstSwap = 0;
    s.json.clear();
    pending.store(true, std::memory_order_release);
    return true;
}

inline std::wstring Status()
{
    auto& s = Get();
    std::lock_guard lock(s.mutex);
    if (s.remaining) return L"Capturing " + std::to_wstring(s.requested - s.remaining) + L"/" + std::to_wstring(s.requested);
    if (s.requested) return L"Captured " + std::to_wstring(s.captured) + L"/" + std::to_wstring(s.requested) + L" frames";
    return {};
}

inline bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(out);
}

inline std::string HashText(uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// Called by the GPU command processor before it presents swap `swap`. When a
// sample is due, stores the frontbuffer readback (passed in by the caller,
// which owns the renderer) and returns true: the caller then presents with a
// capture ticket and calls Complete with its result.
inline bool Begin(uint32_t swap, uint32_t frontbuffer, std::vector<uint32_t>&& frontPixels,
    uint32_t frontWidth, uint32_t frontHeight, bool frontAvailable, std::string planText)
{
    auto& s = Get();
    std::lock_guard lock(s.mutex);
    if (!s.remaining) { pending.store(false, std::memory_order_release); return false; }
    if (!s.firstSwap) s.firstSwap = swap;
    s.activeSwap = swap;
    s.frontbuffer = std::move(frontPixels);
    s.frontWidth = frontWidth; s.frontHeight = frontHeight; s.frontAddress = frontbuffer;
    s.frontAvailable = frontAvailable;
    s.planText = std::move(planText);
    return true;
}

// Between Begin and Complete: the scene before the upscaler for this swap.
inline void SetScene(bool available, std::vector<uint32_t>&& pixels, uint32_t width, uint32_t height)
{
    auto& s = Get();
    std::lock_guard lock(s.mutex);
    s.sceneAvailable = available && width && height && pixels.size() == size_t(width) * height;
    s.scene = std::move(pixels);
    s.sceneWidth = width; s.sceneHeight = height;
}

// presented: the swapchain image of the same swap (present_capture pixels).
inline void Complete(bool presentedAvailable, const std::vector<uint32_t>& presented,
    uint32_t presentedWidth, uint32_t presentedHeight, std::string_view reason)
{
    auto& s = Get();
    std::lock_guard lock(s.mutex);
    if (!s.remaining) return;
    const uint32_t index = s.requested - s.remaining + 1;
    char stem[64];
    std::snprintf(stem, sizeof(stem), "frame-%02u-swap%u", index, s.activeSwap);
    std::string entry = "{\"index\":" + std::to_string(index) + ",\"swap\":" + std::to_string(s.activeSwap) +
        ",\"frontbuffer_address\":" + std::to_string(s.frontAddress & 0x1FFFFFFF) + "," + s.planText;
    bool ok = true;
    if (presentedAvailable) {
        const bool written = WriteFile(s.directory / (std::string(stem) + "-presented.png"),
            EncodePng(presented, presentedWidth, presentedHeight));
        ok &= written;
        entry += ",\"presented\":{\"file\":\"" + std::string(stem) + "-presented.png\",\"width\":" + std::to_string(presentedWidth) +
            ",\"height\":" + std::to_string(presentedHeight) + ",\"fnv1a64\":\"" + HashText(Hash(presented)) + "\"}";
    } else {
        ok = false;
        entry += ",\"presented\":{\"unavailable\":\"" + std::string(reason) + "\"}";
    }
    if (s.frontAvailable) {
        ok &= WriteFile(s.directory / (std::string(stem) + "-frontbuffer.png"), EncodePng(s.frontbuffer, s.frontWidth, s.frontHeight));
        entry += ",\"frontbuffer\":{\"file\":\"" + std::string(stem) + "-frontbuffer.png\",\"width\":" + std::to_string(s.frontWidth) +
            ",\"height\":" + std::to_string(s.frontHeight) + ",\"fnv1a64\":\"" + HashText(Hash(s.frontbuffer)) + "\"}";
    } else entry += ",\"frontbuffer\":{\"unavailable\":\"readback\"}";
    if (s.sceneAvailable) {
        ok &= WriteFile(s.directory / (std::string(stem) + "-scene.png"), EncodePng(s.scene, s.sceneWidth, s.sceneHeight));
        entry += ",\"scene\":{\"file\":\"" + std::string(stem) + "-scene.png\",\"width\":" + std::to_string(s.sceneWidth) +
            ",\"height\":" + std::to_string(s.sceneHeight) + ",\"fnv1a64\":\"" + HashText(Hash(s.scene)) + "\"}";
    } else entry += ",\"scene\":{\"unavailable\":\"no upscaler input this frame\"}";
    s.scene.clear();
    s.scene.shrink_to_fit();
    s.sceneAvailable = false;
    entry += "}";
    s.json += (s.json.empty() ? "" : ",\n  ") + entry;
    s.frontbuffer.clear();
    s.frontbuffer.shrink_to_fit();
    if (ok) ++s.captured;
    if (--s.remaining == 0) {
        pending.store(false, std::memory_order_release);
        std::ofstream manifest(s.directory / "frames.json", std::ios::trunc);
        manifest << "{\n\"schema\": \"lostodyssey.frame-samples.v1\",\n\"first_swap\": " << s.firstSwap
                 << ",\n\"requested\": " << s.requested << ",\n\"captured\": " << s.captured
                 << ",\n\"frames\": [\n  " << s.json << "\n]}\n";
    }
}
} // namespace diagnostics::frame_samples
