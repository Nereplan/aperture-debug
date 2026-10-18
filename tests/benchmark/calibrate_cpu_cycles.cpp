// Hardware-specific diagnostic. Run after timed trials, without concurrent builds.
// A calibration is accepted only for an invariant TSC with a stable measured rate
// and an independent busy-thread check of the Windows cycle counters.
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

static uint64_t Tsc() {
    _mm_lfence();
    const uint64_t result = __rdtsc();
    _mm_lfence();
    return result;
}

int main() {
    int info[4]{};
    __cpuid(info, 0x80000000);
    const uint32_t maxExtended = static_cast<uint32_t>(info[0]);
    bool invariant = false;
    if (maxExtended >= 0x80000007) {
        __cpuid(info, 0x80000007);
        invariant = (info[3] & (1 << 8)) != 0;
    }
    char brand[49]{};
    if (maxExtended >= 0x80000004) for (unsigned i=0; i<3; ++i) {
        __cpuid(info, 0x80000002+i);
        std::memcpy(brand+i*16,info,16);
    }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency)) return 2;
    double rates[5]{};
    uint64_t cycleDeltas[5]{}, qpcDeltas[5]{};
    for (int i=0; i<5; ++i) {
        LARGE_INTEGER before{}, after{};
        QueryPerformanceCounter(&before);
        const uint64_t first = Tsc();
        Sleep(250);
        const uint64_t last = Tsc();
        QueryPerformanceCounter(&after);
        cycleDeltas[i] = last-first;
        qpcDeltas[i] = after.QuadPart-before.QuadPart;
        rates[i] = static_cast<double>(cycleDeltas[i])*frequency.QuadPart/qpcDeltas[i];
    }
    double sorted[5]; std::copy(rates,rates+5,sorted); std::sort(sorted,sorted+5);
    const double rate = sorted[2];
    const double spread = (sorted[4]-sorted[0])/rate;
    ULONG64 threadBefore=0, threadAfter=0, processBefore=0, processAfter=0;
    LARGE_INTEGER start{}, stop{}, current{};
    if (!QueryProcessCycleTime(GetCurrentProcess(),&processBefore) ||
        !QueryThreadCycleTime(GetCurrentThread(),&threadBefore)) return 3;
    QueryPerformanceCounter(&start);
    volatile uint64_t work = 1;
    do {
        work = work*UINT64_C(6364136223846793005)+1;
        QueryPerformanceCounter(&current);
    } while (current.QuadPart-start.QuadPart < frequency.QuadPart/4);
    QueryPerformanceCounter(&stop);
    if (!QueryThreadCycleTime(GetCurrentThread(),&threadAfter) ||
        !QueryProcessCycleTime(GetCurrentProcess(),&processAfter)) return 3;
    const double wall = static_cast<double>(stop.QuadPart-start.QuadPart)/frequency.QuadPart;
    const double threadRatio = (threadAfter-threadBefore)/rate/wall;
    const double processRatio = (processAfter-processBefore)/rate/wall;
    const bool supported = invariant && rate > 0 && spread < 0.005 &&
        threadRatio > 0.9 && threadRatio < 1.02 && processRatio > 0.9 && processRatio < 1.02;
    printf("{\"supported\":%s,\"invariant_tsc\":%s,\"cpu\":\"%s\",\"logical_processors\":%lu,"
        "\"qpc_frequency\":%lld,\"tsc_ticks_per_second\":%.6f,\"relative_rate_spread\":%.9f,"
        "\"busy_wall_seconds\":%.9f,\"thread_busy_ratio\":%.9f,\"process_busy_ratio\":%.9f,"
        "\"thread_cycles\":%llu,\"process_cycles\":%llu,\"uptime_ms\":%llu,\"samples\":[",
        supported?"true":"false", invariant?"true":"false", brand,
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), static_cast<long long>(frequency.QuadPart),
        rate,spread,wall,threadRatio,processRatio,
        static_cast<unsigned long long>(threadAfter-threadBefore),
        static_cast<unsigned long long>(processAfter-processBefore),
        static_cast<unsigned long long>(GetTickCount64()));
    for (int i=0; i<5; ++i) {
        if (i) printf(",");
        printf("{\"tsc_delta\":%llu,\"qpc_delta\":%llu,\"ticks_per_second\":%.6f}",
            static_cast<unsigned long long>(cycleDeltas[i]),static_cast<unsigned long long>(qpcDeltas[i]),rates[i]);
    }
    printf("]}\n");
    return supported?0:4;
}
