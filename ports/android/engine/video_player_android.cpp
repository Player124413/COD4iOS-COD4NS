// Cutscene video playback on Android.
//
// Implements the C API in ports/ios/engine/video_player_apple.h, so the
// shared cinematic code (ports/ios/engine/cinematic_apple.cpp) runs unchanged
// on both platforms. The iOS side uses AVFoundation; here it is the NDK media
// API - AMediaExtractor to demux and AMediaCodec to decode - which is
// hardware-accelerated on every device the port targets and costs almost no
// CPU, which matters because a cutscene still has to hold 60 fps while the
// engine draws the subtitle layer over it.
//
// Output is planar 8-bit 4:2:0, which is what the engine's cinematic material
// samples directly (CINEMATIC_Y/CR/CB). MediaCodec hands back one of several
// YUV layouts depending on the vendor, so the frame is normalised into three
// tightly-packed planes.
//
// Audio: the decoder here is video-only. The cutscene audio track is played
// by the engine's own sound path from the converted .mp3 the conversion
// script emits alongside the video, so the mixer stays in one place and the
// volume slider keeps working.

#include "../../ios/engine/video_player_apple.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KisakCOD-video", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KisakCOD-video", __VA_ARGS__)

namespace {

// MediaCodec colour formats, from android.graphics.ImageFormat and
// OMX_COLOR_FORMATTYPE. The NDK does not define these.
constexpr int32_t kColorFormatYUV420Planar = 19;          // I420: Y, U, V
constexpr int32_t kColorFormatYUV420SemiPlanar = 21;      // NV12: Y, interleaved UV
constexpr int32_t kColorFormatYUV420PackedSemiPlanar = 39;
constexpr int32_t kColorFormatQCOMTiled = 0x7FA30C03;     // older Adreno
constexpr int32_t kColorFormatYUV420Flexible = 0x7F420888;

// Dequeue timeout. Long enough that a slow decoder is given a chance, short
// enough that a stalled one does not hold the frame loop: the caller is on
// the engine thread and a missed cutscene frame is better than a hitch.
constexpr int64_t kDequeueTimeoutUs = 4000;

int64_t NowUs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

} // namespace

struct KisakVideoPlayer
{
    AMediaExtractor *extractor = nullptr;
    AMediaCodec *codec = nullptr;

    int width = 0;
    int height = 0;
    int stride = 0;
    int sliceHeight = 0;
    int32_t colorFormat = kColorFormatYUV420Planar;
    // The decoder may output a buffer larger than the picture; the crop
    // rectangle says which part is real. Ignoring it shows green borders.
    int cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;

    bool inputDone = false;
    bool outputDone = false;
    bool paused = false;
    bool haveFrame = false;

    // Normalised output planes.
    std::vector<uint8_t> planeY;
    std::vector<uint8_t> planeU;
    std::vector<uint8_t> planeV;
    int lumaWidth = 0, lumaHeight = 0;
    int chromaWidth = 0, chromaHeight = 0;

    // Presentation clock, so playback runs at the video's own rate rather
    // than as fast as the decoder can go.
    int64_t startUs = 0;
    int64_t pausedAtUs = 0;
    int64_t presentationUs = 0;
    float volume = 1.0f;
};

namespace {

void ApplyOutputFormat(KisakVideoPlayer *player, AMediaFormat *format)
{
    if (!format)
        return;

    int32_t value = 0;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &value))
        player->width = value;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &value))
        player->height = value;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &value))
        player->colorFormat = value;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &value) && value > 0)
        player->stride = value;
    else
        player->stride = player->width;
    // AMEDIAFORMAT_KEY_SLICE_HEIGHT is __INTRODUCED_IN(28) and minSdk is 26.
    // The constant is only ever the string below, and AMediaFormat_getInt32
    // looks the key up by name, so using it directly costs nothing and works
    // on every API level - the same thing the crop keys do just below.
    if (AMediaFormat_getInt32(format, "slice-height", &value) && value > 0)
        player->sliceHeight = value;
    else
        player->sliceHeight = player->height;

    // "crop-left" and friends; the NDK key constants only exist from API 28,
    // so the strings are used directly.
    int32_t left = 0, top = 0, right = player->width - 1, bottom = player->height - 1;
    AMediaFormat_getInt32(format, "crop-left", &left);
    AMediaFormat_getInt32(format, "crop-top", &top);
    AMediaFormat_getInt32(format, "crop-right", &right);
    AMediaFormat_getInt32(format, "crop-bottom", &bottom);
    player->cropLeft = left;
    player->cropTop = top;
    player->cropRight = right;
    player->cropBottom = bottom;

    player->lumaWidth = std::max(1, right - left + 1);
    player->lumaHeight = std::max(1, bottom - top + 1);
    // 4:2:0 chroma, rounded up so an odd-sized picture still has a full plane.
    player->chromaWidth = (player->lumaWidth + 1) / 2;
    player->chromaHeight = (player->lumaHeight + 1) / 2;

    player->planeY.assign(static_cast<std::size_t>(player->lumaWidth) * player->lumaHeight, 16);
    player->planeU.assign(static_cast<std::size_t>(player->chromaWidth) * player->chromaHeight, 128);
    player->planeV.assign(static_cast<std::size_t>(player->chromaWidth) * player->chromaHeight, 128);

    LOGI("video %dx%d (crop %dx%d), stride %d, slice %d, colour 0x%x", player->width, player->height,
         player->lumaWidth, player->lumaHeight, player->stride, player->sliceHeight, player->colorFormat);
}

// Copies a decoder output buffer into the three tightly-packed planes,
// handling the layouts Android decoders actually produce.
bool ExtractPlanes(KisakVideoPlayer *player, const uint8_t *source, std::size_t size)
{
    const int stride = player->stride > 0 ? player->stride : player->width;
    const int slice = player->sliceHeight > 0 ? player->sliceHeight : player->height;
    const std::size_t lumaSize = static_cast<std::size_t>(stride) * slice;

    if (size < lumaSize)
    {
        LOGE("decoder buffer too small: %zu < %zu", size, lumaSize);
        return false;
    }

    // Luma: copy the cropped rectangle row by row.
    for (int y = 0; y < player->lumaHeight; ++y)
    {
        const uint8_t *row = source + static_cast<std::size_t>(y + player->cropTop) * stride + player->cropLeft;
        std::memcpy(&player->planeY[static_cast<std::size_t>(y) * player->lumaWidth], row,
                    static_cast<std::size_t>(player->lumaWidth));
    }

    const int chromaStride = stride / 2;
    const int cropChromaLeft = player->cropLeft / 2;
    const int cropChromaTop = player->cropTop / 2;

    switch (player->colorFormat)
    {
    case kColorFormatYUV420Planar:
    {
        const std::size_t chromaPlaneSize = lumaSize / 4;
        if (size < lumaSize + chromaPlaneSize * 2)
            return false;
        const uint8_t *u = source + lumaSize;
        const uint8_t *v = source + lumaSize + chromaPlaneSize;
        for (int y = 0; y < player->chromaHeight; ++y)
        {
            const std::size_t offset = static_cast<std::size_t>(y + cropChromaTop) * chromaStride + cropChromaLeft;
            std::memcpy(&player->planeU[static_cast<std::size_t>(y) * player->chromaWidth], u + offset,
                        static_cast<std::size_t>(player->chromaWidth));
            std::memcpy(&player->planeV[static_cast<std::size_t>(y) * player->chromaWidth], v + offset,
                        static_cast<std::size_t>(player->chromaWidth));
        }
        return true;
    }

    case kColorFormatYUV420SemiPlanar:
    case kColorFormatYUV420PackedSemiPlanar:
    case kColorFormatQCOMTiled:
    case kColorFormatYUV420Flexible:
    default:
    {
        // NV12: a single interleaved UV plane. This is what almost every
        // hardware decoder returns, so the de-interleave below is the hot
        // path - it is one pass over half the picture and does not show up in
        // a profile next to the decode itself.
        if (size < lumaSize + lumaSize / 2)
            return false;
        const uint8_t *uv = source + lumaSize;
        for (int y = 0; y < player->chromaHeight; ++y)
        {
            const uint8_t *row = uv + static_cast<std::size_t>(y + cropChromaTop) * stride + player->cropLeft;
            uint8_t *outU = &player->planeU[static_cast<std::size_t>(y) * player->chromaWidth];
            uint8_t *outV = &player->planeV[static_cast<std::size_t>(y) * player->chromaWidth];
            for (int x = 0; x < player->chromaWidth; ++x)
            {
                outU[x] = row[x * 2];
                outV[x] = row[x * 2 + 1];
            }
        }
        return true;
    }
    }
}

void FeedInput(KisakVideoPlayer *player)
{
    if (player->inputDone)
        return;

    const ssize_t index = AMediaCodec_dequeueInputBuffer(player->codec, 0);
    if (index < 0)
        return;

    std::size_t capacity = 0;
    uint8_t *buffer = AMediaCodec_getInputBuffer(player->codec, static_cast<size_t>(index), &capacity);
    if (!buffer)
        return;

    const ssize_t read = AMediaExtractor_readSampleData(player->extractor, buffer, capacity);
    if (read < 0)
    {
        // End of stream: the flag tells the decoder to flush out whatever it
        // is still holding.
        AMediaCodec_queueInputBuffer(player->codec, static_cast<size_t>(index), 0, 0, 0,
                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        player->inputDone = true;
        return;
    }

    const int64_t presentation = AMediaExtractor_getSampleTime(player->extractor);
    AMediaCodec_queueInputBuffer(player->codec, static_cast<size_t>(index), 0, static_cast<size_t>(read),
                                 presentation, 0);
    AMediaExtractor_advance(player->extractor);
}

} // namespace

extern "C" {

KisakVideoPlayer *KisakVideo_Open(const char *path, float volume)
{
    if (!path || !*path)
        return nullptr;

    FILE *probe = std::fopen(path, "rb");
    if (!probe)
    {
        // Not an error worth a log line at warning level: a movie with no
        // converted file reports finished immediately, exactly as a failed
        // BinkOpen did, and the menu or level transition carries on.
        return nullptr;
    }
    std::fclose(probe);

    auto *player = new KisakVideoPlayer();
    player->volume = volume;

    player->extractor = AMediaExtractor_new();
    if (!player->extractor)
    {
        delete player;
        return nullptr;
    }

    FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        AMediaExtractor_delete(player->extractor);
        delete player;
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    const int fd = fileno(file);

    media_status_t status = AMediaExtractor_setDataSourceFd(player->extractor, fd, 0,
                                                            static_cast<off64_t>(length));
    if (status != AMEDIA_OK)
    {
        LOGE("AMediaExtractor_setDataSourceFd(%s): %d", path, status);
        std::fclose(file);
        AMediaExtractor_delete(player->extractor);
        delete player;
        return nullptr;
    }

    const size_t tracks = AMediaExtractor_getTrackCount(player->extractor);
    int videoTrack = -1;
    AMediaFormat *videoFormat = nullptr;
    for (size_t i = 0; i < tracks; ++i)
    {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(player->extractor, i);
        const char *mime = nullptr;
        if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) && mime &&
            std::strncmp(mime, "video/", 6) == 0)
        {
            videoTrack = static_cast<int>(i);
            videoFormat = format;
            break;
        }
        AMediaFormat_delete(format);
    }

    if (videoTrack < 0 || !videoFormat)
    {
        LOGE("%s has no video track", path);
        std::fclose(file);
        AMediaExtractor_delete(player->extractor);
        delete player;
        return nullptr;
    }

    AMediaExtractor_selectTrack(player->extractor, static_cast<size_t>(videoTrack));

    const char *mime = nullptr;
    AMediaFormat_getString(videoFormat, AMEDIAFORMAT_KEY_MIME, &mime);
    player->codec = AMediaCodec_createDecoderByType(mime);
    if (!player->codec)
    {
        LOGE("no decoder for %s", mime);
        AMediaFormat_delete(videoFormat);
        std::fclose(file);
        AMediaExtractor_delete(player->extractor);
        delete player;
        return nullptr;
    }

    // A null surface means buffer output, which is what the engine needs:
    // the planes are uploaded as textures and sampled by the cinematic
    // material, not composited by SurfaceFlinger.
    status = AMediaCodec_configure(player->codec, videoFormat, nullptr, nullptr, 0);
    if (status != AMEDIA_OK)
    {
        LOGE("AMediaCodec_configure: %d", status);
        AMediaCodec_delete(player->codec);
        AMediaFormat_delete(videoFormat);
        std::fclose(file);
        AMediaExtractor_delete(player->extractor);
        delete player;
        return nullptr;
    }

    ApplyOutputFormat(player, videoFormat);
    AMediaFormat_delete(videoFormat);

    if (AMediaCodec_start(player->codec) != AMEDIA_OK)
    {
        LOGE("AMediaCodec_start failed");
        AMediaCodec_delete(player->codec);
        std::fclose(file);
        AMediaExtractor_delete(player->extractor);
        delete player;
        return nullptr;
    }

    // The extractor keeps its own duplicate of the descriptor, so the FILE
    // can be closed now.
    std::fclose(file);

    player->startUs = NowUs();
    LOGI("playing %s", path);
    return player;
}

void KisakVideo_Close(KisakVideoPlayer *player)
{
    if (!player)
        return;
    if (player->codec)
    {
        AMediaCodec_stop(player->codec);
        AMediaCodec_delete(player->codec);
    }
    if (player->extractor)
        AMediaExtractor_delete(player->extractor);
    delete player;
}

int KisakVideo_NextFrame(KisakVideoPlayer *player, KisakVideoFrame *frame)
{
    if (!player || !frame || !player->codec)
        return 0;

    if (player->paused)
        return 0;

    // Do not run ahead of the presentation clock. Without this the decoder
    // would deliver the whole movie in a few hundred milliseconds.
    const int64_t elapsed = NowUs() - player->startUs;
    if (player->haveFrame && player->presentationUs > elapsed)
    {
        // The current frame still stands; tell the caller nothing changed so
        // it reuses the uploaded textures instead of re-uploading them.
        return 0;
    }

    bool produced = false;
    // Bounded: a decoder that needs more input than this in one call is
    // either starting up or seeking, and the frame loop will come back.
    for (int attempt = 0; attempt < 8 && !produced && !player->outputDone; ++attempt)
    {
        FeedInput(player);

        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(player->codec, &info, kDequeueTimeoutUs);

        if (index >= 0)
        {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
                player->outputDone = true;

            if (info.size > 0)
            {
                std::size_t capacity = 0;
                const uint8_t *buffer =
                    AMediaCodec_getOutputBuffer(player->codec, static_cast<size_t>(index), &capacity);
                if (buffer && ExtractPlanes(player, buffer + info.offset, static_cast<std::size_t>(info.size)))
                {
                    player->presentationUs = info.presentationTimeUs;
                    player->haveFrame = true;
                    produced = true;
                }
            }
            AMediaCodec_releaseOutputBuffer(player->codec, static_cast<size_t>(index), false);
        }
        else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            // Always arrives once before the first frame, and the real
            // stride, slice height and crop only exist here.
            AMediaFormat *format = AMediaCodec_getOutputFormat(player->codec);
            ApplyOutputFormat(player, format);
            AMediaFormat_delete(format);
        }
        else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        {
            break; // nothing ready; try again next engine frame
        }
    }

    if (!player->haveFrame)
        return 0;

    frame->planes[0] = player->planeY.data();
    frame->planes[1] = player->planeU.data();
    frame->planes[2] = player->planeV.data();
    frame->strides[0] = static_cast<size_t>(player->lumaWidth);
    frame->strides[1] = static_cast<size_t>(player->chromaWidth);
    frame->strides[2] = static_cast<size_t>(player->chromaWidth);
    frame->widths[0] = player->lumaWidth;
    frame->widths[1] = player->chromaWidth;
    frame->widths[2] = player->chromaWidth;
    frame->heights[0] = player->lumaHeight;
    frame->heights[1] = player->chromaHeight;
    frame->heights[2] = player->chromaHeight;
    return produced ? 1 : 0;
}

int KisakVideo_AtEnd(const KisakVideoPlayer *player)
{
    if (!player)
        return 1;
    return player->outputDone ? 1 : 0;
}

uint32_t KisakVideo_TimeMsec(const KisakVideoPlayer *player)
{
    if (!player)
        return 0;
    return static_cast<uint32_t>(player->presentationUs / 1000);
}

void KisakVideo_SetPaused(KisakVideoPlayer *player, int paused)
{
    if (!player || (paused != 0) == player->paused)
        return;
    player->paused = paused != 0;
    if (player->paused)
    {
        player->pausedAtUs = NowUs();
    }
    else
    {
        // Shift the clock forward by the pause, so resuming does not make the
        // decoder think it is far behind and dump frames to catch up.
        player->startUs += NowUs() - player->pausedAtUs;
    }
}

} // extern "C"
