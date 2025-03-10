/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/CodecID.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace Media::FFmpeg {

static inline AVCodecID ffmpeg_codec_id_from_media_codec_id(CodecID codec)
{
    switch (codec) {
    case CodecID::VP8:
        return AV_CODEC_ID_VP8;
    case CodecID::VP9:
        return AV_CODEC_ID_VP9;
    case CodecID::H261:
        return AV_CODEC_ID_H261;
    case CodecID::MPEG1:
    case CodecID::H262:
        return AV_CODEC_ID_MPEG2VIDEO;
    case CodecID::H263:
        return AV_CODEC_ID_H263;
    case CodecID::H264:
        return AV_CODEC_ID_H264;
    case CodecID::H265:
        return AV_CODEC_ID_HEVC;
    case CodecID::AV1:
        return AV_CODEC_ID_AV1;
    case CodecID::Theora:
        return AV_CODEC_ID_THEORA;
    case CodecID::Vorbis:
        return AV_CODEC_ID_VORBIS;
    case CodecID::Opus:
        return AV_CODEC_ID_OPUS;
    default:
        return AV_CODEC_ID_NONE;
    }
}

static inline CodecID media_codec_id_from_ffmpeg_codec_id(AVCodecID codec)
{
    switch (codec) {
    case AV_CODEC_ID_VP8:
        return CodecID::VP8;
    case AV_CODEC_ID_VP9:
        return CodecID::VP9;
    case AV_CODEC_ID_H261:
        return CodecID::H261;
    case AV_CODEC_ID_MPEG2VIDEO:
        // FIXME: This could also map to CodecID::MPEG1
        return CodecID::H262;
    case AV_CODEC_ID_H263:
        return CodecID::H263;
    case AV_CODEC_ID_H264:
        return CodecID::H264;
    case AV_CODEC_ID_HEVC:
        return CodecID::H265;
    case AV_CODEC_ID_AV1:
        return CodecID::AV1;
    case AV_CODEC_ID_THEORA:
        return CodecID::Theora;
    case AV_CODEC_ID_VORBIS:
        return CodecID::Vorbis;
    case AV_CODEC_ID_OPUS:
        return CodecID::Opus;
    default:
        return CodecID::Unknown;
    }
}

}
