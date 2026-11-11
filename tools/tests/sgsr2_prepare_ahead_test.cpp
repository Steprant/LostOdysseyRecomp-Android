// Steprant 2.0.2 B5: when the renderer prepares SGSR2's session ahead of the
// first frame that uses it (gpu/sgsr2_prepare_ahead.h).
#include <gpu/sgsr2_prepare_ahead.h>
#include <cstdio>
#include <stdexcept>
using namespace gpu::sgsr2_ahead;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        Check(Enabled(nullptr) && Enabled("1") && !Enabled("0"), "LO_SGSR2_PREPARE_AHEAD=0 only turns it off");
        const Plan phone{true, 2003, 900, 2670, 1200, 1};
        const Plan tv{true, 1600, 900, 1920, 1080, 1};
        {
            Tracker t;
            Check(t.Due(true, phone, 0, false), "the first frame start of a plan prepares it");
            for (int frame = 0; frame < 1000; ++frame)
                Check(!t.Due(true, phone, 0, true) && !t.Due(true, phone, 0, false), "once per plan");
            Check(!t.Due(true, tv, 0, true), "another plan while a session exists is left to the renderer's own change of size");
            Check(!t.Due(true, tv, 0, false), "and is not retried");
            t.Released();
            Check(t.Due(true, tv, 0, false), "after the images were released the next frame start prepares the new size");
            Check(!t.Due(true, tv, 0, false), "once");
            t.Released();
            Check(t.Due(true, phone, 0, false), "back to the phone");
        }
        {
            Tracker t;
            Check(!t.Due(true, phone, 3, false), "never after a draw of the frame");
            Check(t.Due(true, phone, 0, false), "the next frame start still prepares it");
        }
        {
            Tracker t;
            Check(!t.Due(false, phone, 0, false) && !t.any, "off: nothing, as in 2.0");
            Plan other = phone; other.sgsr2 = false;
            Check(!t.Due(true, other, 0, false) && !t.any, "a plan without SGSR2 prepares nothing");
            Check(!t.Due(true, Plan{true, 0, 900, 2670, 1200, 1}, 0, false), "no size, nothing");
            Check(!t.Due(true, Plan{true, 2003, 900, 0, 0, 1}, 0, false), "no output size, nothing");
            Check(!t.Due(true, Plan{true, 4000, 900, 2670, 1200, 1}, 0, false), "a render size above the output is not a session");
            Check(!t.any, "invalid plans are not remembered");
        }
        {
            // A session that exists when the plan is first seen (made by a
            // frame in the 2.0 way) is not touched, and not retried later.
            Tracker t;
            Check(!t.Due(true, phone, 0, true) && t.any, "an existing session is left alone");
            Check(!t.Due(true, phone, 0, false), "and the plan counts as attempted");
            Plan epoch = phone; epoch.deviceEpoch = 2;
            Check(t.Due(true, epoch, 0, false), "a new device epoch is a new plan");
        }
        std::printf("sgsr2 prepare ahead: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sgsr2 prepare ahead test failed: %s\n", e.what());
        return 1;
    }
}
