/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/kmalloc.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

// The decoders reach into these structs directly, so a libavcodec they run against must lay them out
// this way. Every major from 60 through 63 does; a newer one is refused until it has been checked.
namespace Media::FFmpeg {

constexpr unsigned LOWEST_SUPPORTED_LIBAVCODEC_MAJOR = 60;
constexpr unsigned HIGHEST_VERIFIED_LIBAVCODEC_MAJOR = 63;

}

static_assert(sizeof(void*) != 8 || (offsetof(AVPacket, pts) == 8 && offsetof(AVPacket, dts) == 16 && offsetof(AVPacket, data) == 24 && offsetof(AVPacket, size) == 32 && offsetof(AVPacket, duration) == 64));
static_assert(sizeof(void*) != 8 || (offsetof(AVFrame, data) == 0 && offsetof(AVFrame, linesize) == 64 && offsetof(AVFrame, extended_data) == 96 && offsetof(AVFrame, width) == 104 && offsetof(AVFrame, height) == 108 && offsetof(AVFrame, nb_samples) == 112 && offsetof(AVFrame, format) == 116 && offsetof(AVFrame, pts) == 136));
static_assert(sizeof(void*) != 8 || (offsetof(AVCodecParameters, codec_type) == 0 && offsetof(AVCodecParameters, codec_id) == 4 && offsetof(AVCodecParameters, extradata) == 16 && offsetof(AVCodecParameters, extradata_size) == 24));
static_assert(sizeof(void*) != 8 || (sizeof(AVChannelLayout) == 24 && offsetof(AVChannelLayout, nb_channels) == 4 && offsetof(AVChannelLayout, u) == 8));

#define FFMPEG_ENUMERATE_FUNCTIONS(F)       \
    F(avcodec_version)                      \
    F(avcodec_find_decoder)                 \
    F(avcodec_alloc_context3)               \
    F(avcodec_free_context)                 \
    F(avcodec_open2)                        \
    F(avcodec_send_packet)                  \
    F(avcodec_receive_frame)                \
    F(avcodec_flush_buffers)                \
    F(avcodec_parameters_alloc)             \
    F(avcodec_parameters_free)              \
    F(avcodec_parameters_to_context)        \
    F(av_packet_alloc)                      \
    F(av_packet_free)                       \
    F(av_new_packet)                        \
    F(av_packet_free_side_data)             \
    F(av_packet_new_side_data)              \
    F(av_frame_alloc)                       \
    F(av_frame_free)                        \
    F(av_frame_unref)                       \
    F(av_malloc)                            \
    F(av_opt_set_int)                       \
    F(av_opt_set_chlayout)                  \
    F(av_opt_get_int)                       \
    F(av_opt_get_chlayout)                  \
    F(av_channel_layout_custom_init)        \
    F(av_channel_layout_channel_from_index) \
    F(av_channel_layout_uninit)             \
    F(av_sample_fmt_is_planar)              \
    F(av_get_planar_sample_fmt)

namespace Media::FFmpeg {

// Every libavcodec and libavutil entry point the decoders call, so they can run against a copy loaded at runtime.
struct FFmpegFunctions {
    AK_ALLOC_WITH_KMALLOC;

#define FFMPEG_FUNCTION_POINTER(name) decltype(&::name) name;
    FFMPEG_ENUMERATE_FUNCTIONS(FFMPEG_FUNCTION_POINTER)
#undef FFMPEG_FUNCTION_POINTER

    static FFmpegFunctions const& bundled();
};

}
