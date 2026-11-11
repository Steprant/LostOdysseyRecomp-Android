// Steprant 2.0.8: WAIT_REG_MEM and interrupt statistics (gpu/gpu_sync_stats.h).
#include <gpu/gpu_sync_stats.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static int checks = 0;
static void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

int main()
{
    using namespace gpu::sync_stats;
    {
        WaitTable t;
        t.Add(true, 0xB000, 0.2);
        Check(t.Waits() == 1 && t.LongWaits() == 0 && t.Top().empty(), "short waits only count in the totals");
        t.Add(true, 0xB000, 9.5);
        t.Add(true, 0xB000, 3.0);
        t.Add(false, 0x5DC, 20.0);
        Check(t.Waits() == 4 && t.LongWaits() == 3, "long waits counted");
        Check(t.TotalMs() > 32.6 && t.TotalMs() < 32.8, "total includes every wait");
        const std::string top = t.Top();
        Check(top.rfind("reg 0x5dc n=1 sum=20.0 max=20.0", 0) == 0, "longest total first");
        Check(top.find("mem 0xb000 n=2 sum=12.5 max=9.5") != std::string::npos, "same address merged");
        for (uint32_t i = 0; i < 20; ++i) t.Add(true, 0x1000 + i * 4, 2.0);
        Check(t.Top(8).find("other n=") != std::string::npos, "overflow reported, not lost silently");
        t.Reset();
        Check(t.Waits() == 0 && t.Top().empty(), "reset");
    }
    {
        InterruptStats s;
        Check(s.Text() == "isr n=0", "empty");
        s.Add(0.5, 0.1);
        s.Add(3.5, 0.3);
        Check(s.count == 2 && s.queueOver2Ms == 1 && s.queueMaxMs == 3.5 && s.handlerMaxMs == 0.3, "round trips");
        Check(s.Text().find("queue_avg=2.00") != std::string::npos, "averages");
    }
    std::printf("gpu sync stats: %d checks passed\n", checks);
    return 0;
}
