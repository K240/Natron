// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_PERFORMANCE_TRACE_H
#define NATRON_GPU_PERFORMANCE_TRACE_H
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
namespace Natron
{
namespace Gpu
{
// Opt-in wall-clock diagnostics; normal rendering does not read the clock.
class PerformanceTrace
{
public:
    explicit PerformanceTrace(const char* operation, uint64_t bytes = 0)
        : _operation(operation), _bytes(bytes), _enabled(enabled())
    {
        if (_enabled) {
            _start = std::chrono::steady_clock::now();
        }
    }
    ~PerformanceTrace()
    {
        if (_enabled) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _start).count();
            std::fprintf(stderr, "NATRON_VULKAN_PROFILE %s bytes=%llu ms=%.3f\n", _operation,
                         static_cast<unsigned long long>(_bytes), ms);
        }
    }
    static bool isEnabled()
    {
        return enabled();
    }
private:
    static bool enabled()
    {
        static const bool value = [] { const char *p = std::getenv("NATRON_VULKAN_PROFILE"); return p && p[0] == '1'; }();
        return value;
    }
    const char *_operation;
    uint64_t _bytes;
    bool _enabled;
    std::chrono::steady_clock::time_point _start;
};
}
}
#endif
