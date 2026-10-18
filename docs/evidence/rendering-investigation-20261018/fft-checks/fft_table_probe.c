// Optional native FFmpeg diagnostic; compare complete JSON output between
// archives and fresh processes. It is not a player performance benchmark.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/tx.h>

typedef struct Result {
    int error;
    uint64_t forward_hash, inverse_hash;
    double max_error;
} Result;

static uint64_t hash_bytes(const void *data, size_t bytes)
{
    const unsigned char *p = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static Result transform(enum AVTXType type, int length)
{
    Result result = {0};
    AVTXContext *forward = NULL, *inverse = NULL;
    av_tx_fn fft = NULL, ifft = NULL;
    const size_t sample = type == AV_TX_DOUBLE_FFT ? sizeof(AVComplexDouble) : sizeof(AVComplexFloat);
    const size_t bytes = sample * length;
    void *input = av_mallocz(bytes), *spectrum = av_mallocz(bytes), *restored = av_mallocz(bytes);
    if (!input || !spectrum || !restored) { result.error = -12; goto done; }
    for (int i = 0; i < length; ++i) {
        const double re = ((i * 37LL + 11) % 251 - 125) / 1024.0;
        const double im = ((i * 19LL + 5) % 239 - 119) / 1024.0;
        if (type == AV_TX_FLOAT_FFT) ((AVComplexFloat *)input)[i] = (AVComplexFloat){(float)re, (float)im};
        else if (type == AV_TX_DOUBLE_FFT) ((AVComplexDouble *)input)[i] = (AVComplexDouble){re, im};
        else ((AVComplexInt32 *)input)[i] = (AVComplexInt32){(int32_t)(re * 65536), (int32_t)(im * 65536)};
    }
    result.error = av_tx_init(&forward, &fft, type, 0, length, NULL, 0);
    if (result.error < 0) goto done;
    result.error = av_tx_init(&inverse, &ifft, type, 1, length, NULL, 0);
    if (result.error < 0) goto done;
    fft(forward, spectrum, input, sample);
    result.forward_hash = hash_bytes(spectrum, bytes);
    ifft(inverse, restored, spectrum, sample);
    result.inverse_hash = hash_bytes(restored, bytes);
    if (type != AV_TX_INT32_FFT) for (int i = 0; i < length * 2; ++i) {
        const double expected = type == AV_TX_FLOAT_FFT ? ((float *)input)[i] : ((double *)input)[i];
        const double actual = (type == AV_TX_FLOAT_FFT ? ((float *)restored)[i] : ((double *)restored)[i]) / length;
        const double error = fabs(actual - expected);
        if (!isfinite(actual)) { result.error = -1; break; }
        if (error > result.max_error) result.max_error = error;
    }
    if (result.max_error > (type == AV_TX_FLOAT_FFT ? 0.00001 : 0.000000001)) result.error = -2;
done:
    av_tx_uninit(&forward); av_tx_uninit(&inverse);
    av_free(input); av_free(spectrum); av_free(restored);
    return result;
}

typedef struct Worker {
    HANDLE start;
    enum AVTXType type;
    Result results[3];
} Worker;

static DWORD WINAPI run_worker(void *opaque)
{
    Worker *worker = opaque;
    const int lengths[] = {1024, 8192, 65536};
    if (WaitForSingleObject(worker->start, INFINITE) != WAIT_OBJECT_0) return 1;
    for (int i = 0; i < 3; ++i) worker->results[i] = transform(worker->type, lengths[i]);
    return 0;
}

static int report(const char *phase, int worker, enum AVTXType type, int length, Result result)
{
    printf("{\"phase\":\"%s\",\"worker\":%d,\"type\":%d,\"length\":%d,\"error\":%d,"
           "\"forward_hash\":\"%016llx\",\"inverse_hash\":\"%016llx\",\"max_error\":%.12g}\n",
           phase, worker, type, length, result.error, (unsigned long long)result.forward_hash,
           (unsigned long long)result.inverse_hash, result.max_error);
    return result.error < 0;
}

// This optional fault injection changes only this test executable's import slot.
// Windows and other processes keep their real VirtualProtect implementation.
typedef BOOL (WINAPI *ProtectFn)(LPVOID, SIZE_T, DWORD, PDWORD);
extern ProtectFn __imp_VirtualProtect;
static ProtectFn real_protect;
static unsigned blocked_transitions;
static BOOL WINAPI reject_table_write(LPVOID address, SIZE_T size, DWORD protection, PDWORD old)
{
    MEMORY_BASIC_INFORMATION region;
    if (protection == PAGE_READWRITE && VirtualQuery(address, &region, sizeof(region)) &&
        region.Type == MEM_IMAGE && region.AllocationBase == GetModuleHandleW(NULL)) {
        ++blocked_transitions;
        SetLastError(ERROR_COMMITMENT_LIMIT);
        return FALSE;
    }
    return real_protect(address, size, protection, old);
}

static int check_protection_failure(void)
{
    char flag[2];
    if (!GetEnvironmentVariableA("APERTURE_TEST_LAZY_TX_TABLES", flag, sizeof(flag))) return 6;
    real_protect = __imp_VirtualProtect;
    DWORD old, ignored;
    if (!real_protect(&__imp_VirtualProtect, sizeof(__imp_VirtualProtect), PAGE_READWRITE, &old)) return 7;
    __imp_VirtualProtect = reject_table_write;
    if (!real_protect(&__imp_VirtualProtect, sizeof(__imp_VirtualProtect), old, &ignored)) return 8;
    AVTXContext *context = NULL;
    av_tx_fn function = NULL;
    const int error = av_tx_init(&context, &function, AV_TX_FLOAT_FFT, 0, 65536, NULL, 0);
    av_tx_uninit(&context);
    printf("{\"protection_failure\":true,\"error\":%d,\"blocked_transitions\":%u}\n", error, blocked_transitions);
    return error == AVERROR(ENOMEM) && blocked_transitions > 0 ? 0 : 9;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--fail-protect") == 0) return check_protection_failure();
    if (argc == 2 && strcmp(argv[1], "--scalar") == 0) av_force_cpu_flags(0);
    else if (argc != 1) return 2;
    const enum AVTXType types[] = {AV_TX_FLOAT_FFT, AV_TX_DOUBLE_FFT, AV_TX_INT32_FFT};
    HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL), threads[6] = {0};
    Worker workers[6] = {0};
    if (!start) return 3;
    for (int i = 0; i < 6; ++i) {
        workers[i].start = start; workers[i].type = types[i % 3];
        threads[i] = CreateThread(NULL, 0, run_worker, &workers[i], 0, NULL);
        if (!threads[i]) { SetEvent(start); return 4; }
    }
    SetEvent(start);
    if (WaitForMultipleObjects(6, threads, TRUE, 120000) != WAIT_OBJECT_0) return 5;
    int failed = 0;
    const int concurrent_lengths[] = {1024, 8192, 65536};
    for (int i = 0; i < 6; ++i) {
        DWORD exit_code = 1;
        GetExitCodeThread(threads[i], &exit_code);
        failed += exit_code != 0;
        CloseHandle(threads[i]);
        for (int j = 0; j < 3; ++j) failed += report("concurrent", i, workers[i].type, concurrent_lengths[j], workers[i].results[j]);
    }
    CloseHandle(start);
    // Largest size initializes every power-of-two table; small composite sizes
    // cover the adjacent non-power-of-two tables and their shared boundary pages.
    const int lengths[] = {3, 5, 7, 9, 15, 8, 16, 32, 64, 128, 256, 512,
                           1024, 2048, 4096, 8192, 16384, 32768, 65536, 2097152};
    for (int t = 0; t < 3; ++t) for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
        failed += report("serial", 0, types[t], lengths[i], transform(types[t], lengths[i]));
    return failed ? 1 : 0;
}
