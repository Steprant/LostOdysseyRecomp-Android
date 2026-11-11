#pragma once

#if defined(__ANDROID__)
#include <android/trace.h>
#endif

namespace diagnostics
{
class AndroidTraceScope
{
public:
#if defined(__ANDROID__)
    explicit AndroidTraceScope(const char* name) : active_(ATrace_isEnabled())
    {
        if (active_) ATrace_beginSection(name);
    }
    ~AndroidTraceScope()
    {
        if (active_) ATrace_endSection();
    }
#else
    explicit AndroidTraceScope(const char*) {}
#endif
    AndroidTraceScope(const AndroidTraceScope&) = delete;
    AndroidTraceScope& operator=(const AndroidTraceScope&) = delete;

private:
#if defined(__ANDROID__)
    bool active_;
#endif
};
}
