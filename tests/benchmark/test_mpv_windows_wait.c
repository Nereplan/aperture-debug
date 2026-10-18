// Compile against the pinned mpv source/header and generated config.h.
// Only the Windows sleep boundary is replaced; timeout conversion is production
// code. The separate playback gate exercises real condition-variable signaling.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

static DWORD observed_ms, sleep_error = ERROR_TIMEOUT;
static unsigned starts, ends, sleeps;
static int64_t observed_ns;
static BOOL test_sleep(PCONDITION_VARIABLE cond, PSRWLOCK lock, DWORD ms, ULONG flags)
{
    (void)cond; (void)lock; (void)flags;
    observed_ms = ms; sleeps++;
    SetLastError(sleep_error);
    return sleep_error == ERROR_SUCCESS;
}
#define SleepConditionVariableSRW test_sleep
#include "osdep/threads-win32.h"
#undef SleepConditionVariableSRW

int64_t mp_start_hires_timers(int64_t ns)
{
    observed_ns = ns; starts++;
    return 123;
}
void mp_end_hires_timers(int64_t token)
{
    if (token != 123) ExitProcess(3);
    ends++;
}

int main(void)
{
    const struct { int64_t ns; DWORD ms; } cases[] = {
        {INT64_MIN, 0}, {-1, 0}, {0, 0}, {1, 1}, {999999, 1},
        {1000000, 1}, {1000001, 1}, {1999999, 1}, {2000000, 2},
        {MP_TIME_MS_TO_NS(INFINITE - 1) + 999999, INFINITE - 1},
        {MP_TIME_MS_TO_NS(INFINITE), INFINITE}, {INT64_MAX, INFINITE},
    };
    mp_cond cond = MP_STATIC_COND_INITIALIZER;
    mp_mutex lock = MP_STATIC_MUTEX_INITIALIZER;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (mp_cond_timedwait(&cond, &lock, cases[i].ns) != ETIMEDOUT ||
            observed_ms != cases[i].ms || observed_ns != MP_TIME_MS_TO_NS(cases[i].ms)) {
            fprintf(stderr, "timeout boundary %u failed: got %lu ms, expected %lu\n",
                    i, (unsigned long)observed_ms, (unsigned long)cases[i].ms);
            return 1;
        }
    }
    sleep_error = ERROR_SUCCESS;
    if (mp_cond_timedwait(&cond, &lock, 1) != 0) return 2;
    sleep_error = ERROR_INVALID_PARAMETER;
    if (mp_cond_timedwait(&cond, &lock, 1) != EINVAL) return 2;
    if (starts != sleeps || ends != sleeps || sleeps != 14) return 3;
    puts("PASS 12 production timeout boundaries, signaling/error mapping and balanced timer cleanup");
    return 0;
}
