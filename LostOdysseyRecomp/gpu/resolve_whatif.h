#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Steprant 2.1.5: how many resolve copies could be avoided ("resolve what-if").
//
// A resolve copies a render target (EDRAM) into the texture the game samples
// afterwards. Measurement only: nothing here changes what is recorded. Each
// resolve is followed until the same surface is resolved again (or dropped),
// then counted in one class:
//
//   unread      never read: the copy was not needed at all.
//   free        same host format, the whole surface, source and surface of the
//               same size, and never read after its source was next written:
//               the surface could have been the source image itself.
//   rename      as free, but read after its source was next written by a clear
//               of the whole source: giving the source a fresh image at that
//               clear would avoid the copy.
//   copy        as free, but read after its source was partly written (a draw,
//               a partial clear): a copy is still needed, only later.
//   format      a conversion (other host format: a shader copy).
//   partial     not the whole surface.
//   size        the whole surface, but the source has another size.
//
// Depth resolves count in the same classes (eligible ones would be sampled
// through a depth view of the source). Copies made for cropped fetch views
// (a sampled size smaller than the resolve) are counted apart.
//
// Reads are every lookup of the surface (draw fetches, presentation,
// diagnostics), so "read after write" errs towards "copy": the avoidable
// numbers are a lower bound.
//
// Steprant 2.2: an unread copy can only be skipped by deferring it until
// the surface is read (and dropping it when the surface is resolved again).
// If the source is written in the meantime, the deferred copy has to be made
// at that write, before the source changes: nothing is saved. The line adds
// unread_intact, the unread copies whose source stayed untouched until the
// surface was resolved again or dropped (what deferral would save), and the
// three surfaces with the most unread bytes (destination base, format, share
// intact), to tell which resolves they are.
namespace gpu::resolve_whatif
{
    enum class Kind : uint8_t { ColorCopy, ColorBlit, Depth };
    enum class Outcome : uint8_t { Unread, Free, Rename, Copy, Format, Partial, Size, Count };

    inline const char* OutcomeName(Outcome outcome)
    {
        switch (outcome) {
        case Outcome::Unread: return "unread";
        case Outcome::Free: return "free";
        case Outcome::Rename: return "rename";
        case Outcome::Copy: return "copy";
        case Outcome::Format: return "format";
        case Outcome::Partial: return "partial";
        case Outcome::Size: return "size";
        default: return "?";
        }
    }

    struct Resolve
    {
        uint64_t surface = 0;            // SurfaceKey(destBase, destFormat)
        uint64_t sourceAllocation = 0;   // the source target's allocation serial
        Kind kind = Kind::ColorCopy;
        bool wholeSurface = false;       // the region is the whole destination texture
        bool sameSize = false;           // destination and source have the same size
        uint64_t bytes = 0;              // bytes the resolve reads and writes
    };

    inline uint64_t SurfaceKey(uint32_t destBase, uint32_t destFormat)
    {
        return (uint64_t(destBase) << 32) | destFormat;
    }

    // Bytes a resolve of width x height moves: the source read and the
    // destination write.
    inline uint64_t ResolveBytes(uint32_t width, uint32_t height, uint32_t sourceBpp, uint32_t destBpp)
    {
        return uint64_t(width) * height * (uint64_t(sourceBpp) + destBpp);
    }

    // Where a finished resolve belongs.
    struct Fate
    {
        bool read = false;            // looked up at least once
        bool sourceWritten = false;   // its source was written after it
        bool wholeClear = false;      // that first write was a clear of the whole source
        bool readAfterWrite = false;  // looked up after that write
    };

    inline Outcome Classify(const Resolve& r, const Fate& fate)
    {
        if (!fate.read) return Outcome::Unread;
        if (r.kind == Kind::ColorBlit) return Outcome::Format;
        if (!r.wholeSurface) return Outcome::Partial;
        if (!r.sameSize) return Outcome::Size;
        if (!fate.readAfterWrite) return Outcome::Free;
        return fate.wholeClear ? Outcome::Rename : Outcome::Copy;
    }

    class Tracker
    {
    public:
        struct Totals
        {
            uint64_t frames = 0;
            uint64_t resolves = 0, bytes = 0;
            uint64_t count[size_t(Outcome::Count)] = {};
            uint64_t outcomeBytes[size_t(Outcome::Count)] = {};
            uint64_t kinds[3] = {};
            uint64_t croppedCopies = 0, croppedBytes = 0;
            uint64_t unreadIntact = 0, unreadIntactBytes = 0;
            struct Surface { uint64_t surface = 0, count = 0, bytes = 0, intact = 0; };
            std::vector<Surface> unreadSurfaces;   // at most kMaxSurfaces
        };

        // A new resolve: the previous one of the same surface is finished.
        void OnResolve(const Resolve& r)
        {
            Retire(r.surface);
            if (live.size() >= kMaxLive) Finish(0);
            live.push_back({r, {}});
            watched |= Bit(r.sourceAllocation);
            totals.resolves++;
            totals.bytes += r.bytes;
            totals.kinds[size_t(r.kind)]++;
        }

        // A write to a target (draw binding, clear, transfer). Only the first
        // write after each resolve of it matters.
        void OnSourceWrite(uint64_t allocation, bool wholeClear)
        {
            if (!allocation || !(watched & Bit(allocation))) return;
            for (Live& l : live) {
                if (l.resolve.sourceAllocation != allocation || l.fate.sourceWritten) continue;
                l.fate.sourceWritten = true;
                l.fate.wholeClear = wholeClear;
            }
        }

        // A lookup of a resolved surface.
        void OnRead(uint64_t surface)
        {
            for (Live& l : live) {
                if (l.resolve.surface != surface) continue;
                l.fate.read = true;
                if (l.fate.sourceWritten) l.fate.readAfterWrite = true;
            }
        }

        void OnDrop(uint64_t surface) { Retire(surface); }

        void OnCroppedCopy(uint64_t bytes)
        {
            totals.croppedCopies++;
            totals.croppedBytes += bytes;
        }

        void EndFrame() { totals.frames++; }

        const Totals& Window() const { return totals; }

        // "resolve what-if:" per frame since the last call; empty without frames.
        std::string TakeLine()
        {
            if (totals.frames == 0) return {};
            const double f = double(totals.frames);
            const auto mb = [&](uint64_t b) { return double(b) / (1024.0 * 1024.0) / f; };
            const auto n = [&](uint64_t c) { return double(c) / f; };
            const auto& c = totals.count;
            const auto& b = totals.outcomeBytes;
            const auto at = [](Outcome o) { return size_t(o); };
            const uint64_t avoidable = b[at(Outcome::Unread)] + b[at(Outcome::Free)] + b[at(Outcome::Rename)];
            uint64_t classified = 0;
            for (uint64_t value : b) classified += value;
            char buffer[512];
            std::snprintf(buffer, sizeof(buffer),
                "resolve what-if: frames=%llu per_frame: resolves=%.1f mb=%.1f (copy=%.1f blit=%.1f depth=%.1f) | "
                "avoidable: unread=%.1f/%.1fmb free=%.1f/%.1fmb rename=%.1f/%.1fmb | "
                "kept: copy=%.1f/%.1fmb format=%.1f/%.1fmb partial=%.1f/%.1fmb size=%.1f/%.1fmb | "
                "cropped_views=%.1f/%.1fmb | avoidable_mb=%.1f (%.0f%%)",
                (unsigned long long)totals.frames, n(totals.resolves), mb(totals.bytes),
                n(totals.kinds[0]), n(totals.kinds[1]), n(totals.kinds[2]),
                n(c[at(Outcome::Unread)]), mb(b[at(Outcome::Unread)]),
                n(c[at(Outcome::Free)]), mb(b[at(Outcome::Free)]),
                n(c[at(Outcome::Rename)]), mb(b[at(Outcome::Rename)]),
                n(c[at(Outcome::Copy)]), mb(b[at(Outcome::Copy)]),
                n(c[at(Outcome::Format)]), mb(b[at(Outcome::Format)]),
                n(c[at(Outcome::Partial)]), mb(b[at(Outcome::Partial)]),
                n(c[at(Outcome::Size)]), mb(b[at(Outcome::Size)]),
                n(totals.croppedCopies), mb(totals.croppedBytes),
                mb(avoidable), classified ? 100.0 * double(avoidable) / double(classified) : 0.0);
            std::string line = buffer;
            std::snprintf(buffer, sizeof(buffer), " | unread_intact=%.1f/%.1fmb | unread_top:",
                n(totals.unreadIntact), mb(totals.unreadIntactBytes));
            line += buffer;
            auto top = totals.unreadSurfaces;
            std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.bytes > y.bytes; });
            if (top.empty()) line += " none";
            for (size_t i = 0; i < top.size() && i < 3; i++) {
                std::snprintf(buffer, sizeof(buffer), " base=0x%08x fmt=%u n=%.1f mb=%.1f intact=%.0f%%;",
                    unsigned(top[i].surface >> 32), unsigned(top[i].surface & 0xFFFFFFFFu), n(top[i].count), mb(top[i].bytes),
                    top[i].count ? 100.0 * double(top[i].intact) / double(top[i].count) : 0.0);
                line += buffer;
            }
            totals = Totals();
            return line;
        }

    private:
        struct Live { Resolve resolve; Fate fate; };
        // Surfaces resolved once and never again stay live; the oldest is
        // finished past this many.
        static constexpr size_t kMaxLive = 256;
        static constexpr size_t kMaxSurfaces = 32;

        std::vector<Live> live;
        uint64_t watched = 0;   // a bit per source allocation (mod 64): a quick miss
        Totals totals;

        static uint64_t Bit(uint64_t allocation) { return uint64_t(1) << (allocation & 63); }

        void Finish(size_t index)
        {
            const Live& l = live[index];
            const Outcome outcome = Classify(l.resolve, l.fate);
            totals.count[size_t(outcome)]++;
            totals.outcomeBytes[size_t(outcome)] += l.resolve.bytes;
            if (outcome == Outcome::Unread) {
                const bool intact = !l.fate.sourceWritten;
                if (intact) {
                    totals.unreadIntact++;
                    totals.unreadIntactBytes += l.resolve.bytes;
                }
                auto& surfaces = totals.unreadSurfaces;
                auto it = std::find_if(surfaces.begin(), surfaces.end(), [&](const auto& x) { return x.surface == l.resolve.surface; });
                if (it == surfaces.end() && surfaces.size() < kMaxSurfaces)
                    it = surfaces.insert(surfaces.end(), Totals::Surface{l.resolve.surface});
                if (it != surfaces.end()) {
                    it->count++;
                    it->bytes += l.resolve.bytes;
                    it->intact += intact;
                }
            }
            live.erase(live.begin() + ptrdiff_t(index));
            RebuildWatched();
        }

        void Retire(uint64_t surface)
        {
            for (size_t i = 0; i < live.size(); i++) {
                if (live[i].resolve.surface == surface) { Finish(i); return; }
            }
        }

        void RebuildWatched()
        {
            watched = 0;
            for (const Live& l : live) watched |= Bit(l.resolve.sourceAllocation);
        }
    };
}
