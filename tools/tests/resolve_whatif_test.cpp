// Steprant 2.1.5: the resolve what-if classes (gpu/resolve_whatif.h).
#include <gpu/resolve_whatif.h>

#include <cassert>
#include <cstdio>
#include <string>

using namespace gpu::resolve_whatif;

namespace
{
    Resolve Make(uint32_t base, uint64_t source, Kind kind = Kind::ColorCopy, bool whole = true, bool sameSize = true, uint64_t bytes = 100)
    {
        Resolve r;
        r.surface = SurfaceKey(base, 6);
        r.sourceAllocation = source;
        r.kind = kind;
        r.wholeSurface = whole;
        r.sameSize = sameSize;
        r.bytes = bytes;
        return r;
    }

    uint64_t Count(const Tracker& t, Outcome o) { return t.Window().count[size_t(o)]; }
    uint64_t Bytes(const Tracker& t, Outcome o) { return t.Window().outcomeBytes[size_t(o)]; }
}

int main()
{
    // Classify, in its order of precedence.
    {
        Fate fate;
        assert(Classify(Make(1, 1), fate) == Outcome::Unread);
        fate.read = true;
        assert(Classify(Make(1, 1, Kind::ColorBlit), fate) == Outcome::Format);
        assert(Classify(Make(1, 1, Kind::ColorCopy, false), fate) == Outcome::Partial);
        assert(Classify(Make(1, 1, Kind::ColorCopy, true, false), fate) == Outcome::Size);
        assert(Classify(Make(1, 1), fate) == Outcome::Free);
        fate.sourceWritten = true;
        assert(Classify(Make(1, 1), fate) == Outcome::Free);       // read only before the write
        fate.readAfterWrite = true;
        assert(Classify(Make(1, 1), fate) == Outcome::Copy);
        fate.wholeClear = true;
        assert(Classify(Make(1, 1), fate) == Outcome::Rename);
        assert(Classify(Make(1, 1, Kind::Depth), fate) == Outcome::Rename);
    }

    // The life of a resolve, finished by the next resolve of its surface.
    {
        Tracker t;
        const uint64_t a = SurfaceKey(0x100000, 6), b = SurfaceKey(0x200000, 6);
        // Unread.
        t.OnResolve(Make(0x100000, 7));
        t.OnResolve(Make(0x100000, 7));
        assert(Count(t, Outcome::Unread) == 1);
        // Free: read, then its source written.
        t.OnRead(a);
        t.OnSourceWrite(7, false);
        t.OnResolve(Make(0x100000, 7));
        assert(Count(t, Outcome::Free) == 1);
        // Rename: a whole clear of the source, then a read.
        t.OnSourceWrite(7, true);
        t.OnRead(a);
        t.OnResolve(Make(0x100000, 7));
        assert(Count(t, Outcome::Rename) == 1);
        // Copy: a draw into the source first; a later whole clear does not
        // change the first write.
        t.OnSourceWrite(7, false);
        t.OnSourceWrite(7, true);
        t.OnRead(a);
        t.OnDrop(a);
        assert(Count(t, Outcome::Copy) == 1);
        // Writes to another target and reads of another surface change nothing;
        // two surfaces resolved from one source both see its write.
        t.OnResolve(Make(0x100000, 7));
        t.OnResolve(Make(0x200000, 7));
        t.OnSourceWrite(7 + 64, false);   // same quick-miss bit, other allocation
        t.OnRead(b);
        t.OnRead(a);
        t.OnSourceWrite(7, false);
        t.OnRead(b);
        t.OnDrop(a);
        t.OnDrop(b);
        assert(Count(t, Outcome::Free) == 2);
        assert(Count(t, Outcome::Copy) == 2);
        // Ineligible ones keep their copy whatever happens.
        t.OnResolve(Make(0x300000, 9, Kind::ColorBlit, true, true, 40));
        t.OnRead(SurfaceKey(0x300000, 6));
        t.OnDrop(SurfaceKey(0x300000, 6));
        assert(Count(t, Outcome::Format) == 1 && Bytes(t, Outcome::Format) == 40);
        // A dropped surface that was never resolved does nothing.
        t.OnDrop(SurfaceKey(0x400000, 6));
        assert(t.Window().resolves == 7);
        assert(t.Window().kinds[size_t(Kind::ColorCopy)] == 6 && t.Window().kinds[size_t(Kind::ColorBlit)] == 1);
    }

    // The line: per frame, then a new window.
    {
        Tracker t;
        assert(t.TakeLine().empty());
        t.OnResolve(Make(0x100000, 1, Kind::ColorCopy, true, true, 4u << 20));
        t.OnRead(SurfaceKey(0x100000, 6));
        t.OnResolve(Make(0x100000, 1, Kind::ColorCopy, true, true, 4u << 20));   // the first is free
        t.OnResolve(Make(0x500000, 2, Kind::ColorBlit, true, true, 2u << 20));
        t.OnRead(SurfaceKey(0x500000, 6));
        t.OnDrop(SurfaceKey(0x500000, 6));
        t.OnCroppedCopy(1u << 20);
        t.EndFrame();
        t.EndFrame();
        const std::string line = t.TakeLine();
        std::printf("%s\n", line.c_str());
        assert(line.rfind("resolve what-if: frames=2 per_frame: resolves=1.5 mb=5.0 (copy=1.0 blit=0.5 depth=0.0) | ", 0) == 0);
        assert(line.find("free=0.5/2.0mb") != std::string::npos);
        assert(line.find("format=0.5/1.0mb") != std::string::npos);
        assert(line.find("cropped_views=0.5/0.5mb") != std::string::npos);
        assert(line.find("avoidable_mb=2.0 (67%)") != std::string::npos);
        assert(t.Window().frames == 0 && t.Window().resolves == 0);
        assert(t.TakeLine().empty());
    }

    // Surfaces never resolved again leave: the oldest is finished at the cap.
    {
        Tracker t;
        for (uint32_t i = 0; i < 300; i++) t.OnResolve(Make(0x1000 * (i + 1), i + 1));
        assert(Count(t, Outcome::Unread) == 300 - 256);
    }

    std::printf("resolve what-if: all checks passed\n");
    return 0;
}
