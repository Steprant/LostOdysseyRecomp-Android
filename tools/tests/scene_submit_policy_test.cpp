// Steprant 2.4.6: when a heavy scene is submitted in pieces.
#include <gpu/scene_submit_policy.h>
#include <cstdio>
#include <cstdlib>

using namespace gpu::scene_submit;

namespace {
int checks = 0;
void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "scene submit policy test failed: %s\n", what); std::exit(1); }
}
}

int main()
{
    const Settings s; // 768 / 1800
    Check(s.pieceDraws == 768 && s.minFrameDraws == 1800, "defaults");

    // Light frames never split.
    Check(Due(s, 1799, 5000, true) == Split::None, "previous frame under the minimum");
    Check(Due(s, 0, 10000, false) == Split::None, "first frame");
    // A heavy previous frame: at a render target change once the piece is full.
    Check(Due(s, 3000, 767, true) == Split::None, "piece not full yet");
    Check(Due(s, 3000, 768, true) == Split::AtTargetChange, "piece full at a target change");
    Check(Due(s, 3000, 768, false) == Split::None, "piece full inside a pass");
    Check(Due(s, 3000, 1535, false) == Split::None, "under twice the piece inside a pass");
    Check(Due(s, 3000, 1536, false) == Split::InsidePass, "twice the piece inside a pass");
    Check(Due(s, 1800, 2000, true) == Split::AtTargetChange, "exactly the minimum");

    // Off and parsing.
    const Settings off = FromValues("0", nullptr);
    Check(off.pieceDraws == 0 && off.minFrameDraws == 1800, "0 turns it off");
    Check(Due(off, 100000, 100000, true) == Split::None, "off never splits");
    const Settings custom = FromValues("500", "1200");
    Check(custom.pieceDraws == 500 && custom.minFrameDraws == 1200, "values");
    Check(Due(custom, 1200, 500, true) == Split::AtTargetChange, "custom piece");
    const Settings bad = FromValues("12x", "");
    Check(bad.pieceDraws == 768 && bad.minFrameDraws == 1800, "invalid values keep the defaults");
    Check(FromValues("-3", "99999999").pieceDraws == 768, "negative value keeps the default");
    Check(FromValues("4294967295", nullptr).pieceDraws == 768, "out of range keeps the default");

    // Statistics.
    Stats st;
    st.Note(Split::None, 5); st.EndFrame();
    st.Note(Split::AtTargetChange, 1000); st.Note(Split::InsidePass, 2000); st.EndFrame();
    st.EndFrame();
    Check(st.frames == 3 && st.splitFrames == 1, "frames and split frames");
    Check(st.Submits() == 2 && st.atTargetChange == 1 && st.insidePass == 1 && st.splitNs == 3000, "submits and time");

    std::printf("scene submit policy test: %d checks passed\n", checks);
    return 0;
}
