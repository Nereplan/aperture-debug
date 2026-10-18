// Isolated diagnostic of mpv's Windows timeout conversion and outer deadline loop.
// Run without builds/profilers; this does not measure player performance.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
LONG NTAPI NtSetTimerResolution(ULONG requested, BOOLEAN set, PULONG actual);
static LARGE_INTEGER frequency;
static int64_t now_ns(void) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return q.QuadPart / frequency.QuadPart * INT64_C(1000000000) +
        q.QuadPart % frequency.QuadPart * INT64_C(1000000000) / frequency.QuadPart;
}
static uint64_t file_ticks(FILETIME time) {
    return ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
}
static uint64_t thread_ticks(void) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) exit(4);
    return file_ticks(kernel) + file_ticks(user);
}
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "floor") && strcmp(argv[1], "ceil"))) return 2;
    const int round_up = !strcmp(argv[1], "ceil");
    QueryPerformanceFrequency(&frequency);
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE cond = CONDITION_VARIABLE_INIT;
    uint64_t waits = 0, zero_waits = 0;
    int64_t lateness_sum = 0, lateness_max = 0;
    const int frames = 150;
    const int64_t period = 1000000000 / 30;
    AcquireSRWLockExclusive(&lock);
    const int64_t start = now_ns();
    const uint64_t cpu_start = thread_ticks();
    for (int frame = 1; frame <= frames; ++frame) {
        const int64_t target = start + period * frame;
        int64_t remaining;
        while ((remaining = target - now_ns()) > 0) {
            // Same positive-duration conversion and per-wait HRT policy as mpv.
            const DWORD millis = (DWORD)((remaining + (round_up ? 999999 : 0)) / 1000000);
            const int hires = millis > 0 && millis <= 50;
            ULONG actual;
            const int acquired = hires && NtSetTimerResolution(10000, TRUE, &actual) >= 0;
            ++waits; zero_waits += millis == 0;
            if (!SleepConditionVariableSRW(&cond, &lock, millis, 0) && GetLastError() != ERROR_TIMEOUT) return 5;
            if (acquired) NtSetTimerResolution(10000, FALSE, &actual);
        }
        const int64_t late = now_ns() - target;
        lateness_sum += late;
        if (late > lateness_max) lateness_max = late;
    }
    const uint64_t cpu_end = thread_ticks();
    const int64_t elapsed = now_ns() - start;
    ReleaseSRWLockExclusive(&lock);
    printf("{\"policy\":\"%s\",\"frames\":%d,\"waits\":%llu,\"zero_timeout_waits\":%llu,"
        "\"thread_cpu_ticks_start\":%llu,\"thread_cpu_ticks_end\":%llu,\"elapsed_ns\":%lld,"
        "\"average_lateness_ns\":%lld,\"max_lateness_ns\":%lld}\n", argv[1], frames,
        (unsigned long long)waits, (unsigned long long)zero_waits,
        (unsigned long long)cpu_start, (unsigned long long)cpu_end, (long long)elapsed,
        (long long)(lateness_sum / frames), (long long)lateness_max);
    return 0;
}
