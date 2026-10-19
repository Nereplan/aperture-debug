// CPU-only demux/decode probe. No window, renderer, hardware decode or audio
// output. Hashing runs separately from timing; this cannot rank whole players.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/sha.h>

struct result {
    int frames;
    int packets;
    uint64_t units; // video pixels or audio samples per channel
    char hash[65];
    char decoder[128];
    double elapsed;
    double cpu_seconds;
    uint64_t cycles;
};

static enum AVPixelFormat software_format(AVCodecContext *context,
                                          const enum AVPixelFormat *formats)
{
    (void)context;
    for (; *formats != AV_PIX_FMT_NONE; formats++) {
        const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(*formats);
        if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *formats;
    }
    return AV_PIX_FMT_NONE;
}

static uint64_t file_ticks(FILETIME value)
{
    return ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
}

static int process_cpu(uint64_t *ticks)
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    *ticks = file_ticks(kernel) + file_ticks(user);
    return 1;
}

static int hash_frame(struct AVSHA *sha, AVFrame *frame, enum AVMediaType kind,
                      uint8_t **buffer, unsigned *capacity)
{
    int header[] = {kind, frame->format, frame->width, frame->height,
        frame->nb_samples, frame->sample_rate, frame->ch_layout.nb_channels,
        frame->color_range, frame->color_primaries, frame->color_trc, frame->colorspace};
    av_sha_update(sha, (const uint8_t *)header, sizeof(header));
    if (kind == AVMEDIA_TYPE_VIDEO) {
        int bytes = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
        if (bytes < 0) return bytes;
        av_fast_malloc(buffer, capacity, bytes);
        if (!*buffer) return AVERROR(ENOMEM);
        int copied = av_image_copy_to_buffer(*buffer, bytes,
            (const uint8_t * const *)frame->data, frame->linesize,
            frame->format, frame->width, frame->height, 1);
        if (copied < 0) return copied;
        av_sha_update(sha, *buffer, copied);
    } else {
        const int planar = av_sample_fmt_is_planar(frame->format);
        const int channels = frame->ch_layout.nb_channels;
        const int bytes = av_get_bytes_per_sample(frame->format);
        if (channels <= 0 || bytes <= 0 || frame->nb_samples < 0) return AVERROR(EINVAL);
        for (int n = 0; n < (planar ? channels : 1); n++)
            av_sha_update(sha, frame->extended_data[n],
                (size_t)frame->nb_samples * bytes * (planar ? 1 : channels));
    }
    return 0;
}

static int decode(const char *path, enum AVMediaType kind, int limit,
                  int hashing, struct result *result)
{
    AVFormatContext *format = NULL;
    AVCodecContext *decoder = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    const AVCodec *codec = NULL;
    struct AVSHA *sha = NULL;
    uint8_t *buffer = NULL;
    unsigned capacity = 0;
    int error, stream;
    memset(result, 0, sizeof(*result));
    if ((error = avformat_open_input(&format, path, NULL, NULL)) < 0) goto done;
    if ((error = avformat_find_stream_info(format, NULL)) < 0) goto done;
    stream = av_find_best_stream(format, kind, -1, -1, &codec, 0);
    if (stream < 0) { error = stream; goto done; }
    decoder = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!decoder || !frame || !packet) { error = AVERROR(ENOMEM); goto done; }
    if ((error = avcodec_parameters_to_context(decoder, format->streams[stream]->codecpar)) < 0) goto done;
    decoder->thread_count = 1;
    decoder->get_format = software_format;
    if ((error = avcodec_open2(decoder, codec, NULL)) < 0) goto done;
    snprintf(result->decoder, sizeof(result->decoder), "%s", codec->name);
    if (hashing) {
        sha = av_sha_alloc();
        if (!sha) { error = AVERROR(ENOMEM); goto done; }
        av_sha_init(sha, 256);
    }
    LARGE_INTEGER frequency, start, end;
    uint64_t cpu_start, cpu_end;
    ULONG64 cycle_start, cycle_end;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&start) ||
        !process_cpu(&cpu_start) || !QueryProcessCycleTime(GetCurrentProcess(), &cycle_start)) {
        error = AVERROR_EXTERNAL; goto done;
    }
    int eof = 0;
    while (result->frames < limit) {
        error = av_read_frame(format, packet);
        if (error == AVERROR_EOF) {
            eof = 1;
            error = avcodec_send_packet(decoder, NULL);
        } else if (error < 0) {
            goto done;
        } else if (packet->stream_index != stream) {
            av_packet_unref(packet);
            continue;
        } else {
            result->packets++;
            error = avcodec_send_packet(decoder, packet);
        }
        av_packet_unref(packet);
        if (error < 0 && error != AVERROR_EOF) goto done;
        while (result->frames < limit) {
            error = avcodec_receive_frame(decoder, frame);
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) break;
            if (error < 0) goto done;
            if (hashing && (error = hash_frame(sha, frame, kind, &buffer, &capacity)) < 0) goto done;
            result->units += kind == AVMEDIA_TYPE_VIDEO
                ? (uint64_t)frame->width * frame->height : frame->nb_samples;
            result->frames++;
            av_frame_unref(frame);
        }
        if (eof) break;
    }
    if (!QueryPerformanceCounter(&end) || !process_cpu(&cpu_end) ||
        !QueryProcessCycleTime(GetCurrentProcess(), &cycle_end)) { error = AVERROR_EXTERNAL; goto done; }
    result->elapsed = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    result->cpu_seconds = (cpu_end - cpu_start) / 10000000.0;
    result->cycles = cycle_end - cycle_start;
    error = result->frames == limit ? 0 : AVERROR_EOF;
    if (hashing && !error) {
        uint8_t digest[32];
        av_sha_final(sha, digest);
        for (int n = 0; n < 32; n++) snprintf(result->hash + n * 2, 3, "%02x", digest[n]);
    }
done:
    av_free(buffer);
    av_free(sha);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return error;
}

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[2], "video") && strcmp(argv[2], "audio"))) {
        fprintf(stderr, "usage: ffmpeg_decode_probe FILE video|audio FRAME_LIMIT\n");
        return 2;
    }
    char *end = NULL;
    long limit = strtol(argv[3], &end, 10);
    if (!end || *end || limit < 1 || limit > 100000) return 2;
    av_log_set_level(AV_LOG_ERROR);
    struct result quality, timing;
    enum AVMediaType kind = !strcmp(argv[2], "video") ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    // Complete decoded-buffer/metadata hashing is a separate warmup. The timed
    // pass uses a new decoder; file open, stream discovery and codec init are
    // outside the counters. Demux, decode and output-frame release are inside.
    int error = decode(argv[1], kind, (int)limit, 1, &quality);
    if (!error) error = decode(argv[1], kind, (int)limit, 0, &timing);
    if (error < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(error, message, sizeof(message));
        fprintf(stderr, "decode failed: %s\n", message); return 1;
    }
    if (quality.frames != timing.frames || quality.units != timing.units ||
        strcmp(quality.decoder, timing.decoder)) return 1;
    printf("{\"kind\":\"%s\",\"decoder\":\"%s\",\"threads\":1,\"hardware\":false,"
           "\"frames\":%d,\"packets\":%d,\"units\":%llu,\"decodedSha256\":\"%s\","
           "\"elapsedSeconds\":%.9f,\"processCpuSeconds\":%.9f,\"processCycles\":%llu,"
           "\"avcodecVersion\":%u,\"windowsCreated\":0}\n",
           argv[2], timing.decoder, timing.frames, timing.packets,
           (unsigned long long)timing.units, quality.hash, timing.elapsed,
           timing.cpu_seconds, (unsigned long long)timing.cycles, avcodec_version());
    return 0;
}
