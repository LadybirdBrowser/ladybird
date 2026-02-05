/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibMedia/CodecID.h>

namespace Media::Matroska {

static constexpr CodecID codec_id_from_matroska_id_string(String const& codec_id)
{
    if (codec_id == "V_VP8")
        return CodecID::VP8;
    if (codec_id == "V_VP9")
        return CodecID::VP9;
    if (codec_id == "V_MPEG1")
        return CodecID::MPEG1;
    if (codec_id == "V_MPEG2")
        return CodecID::H262;
    if (codec_id == "V_MPEG4/ISO/AVC")
        return CodecID::H264;
    if (codec_id == "V_MPEGH/ISO/HEVC")
        return CodecID::H265;
    if (codec_id == "A_MPEG/L3")
        return CodecID::MP3;
    if (codec_id == "A_AAC" || codec_id == "A_AAC/MPEG4/LC"
        || codec_id == "A_AAC/MPEG4/LC/SBR" || codec_id == "A_AAC/MPEG4/LTP"
        || codec_id == "A_AAC/MPEG4/MAIN" || codec_id == "A_AAC/MPEG4/SSR")
        return CodecID::AAC;
    if (codec_id == "V_AV1")
        return CodecID::AV1;
    if (codec_id == "V_THEORA")
        return CodecID::Theora;
    if (codec_id == "A_VORBIS")
        return CodecID::Vorbis;
    if (codec_id == "A_OPUS")
        return CodecID::Opus;
    if (codec_id == "A_FLAC")
        return CodecID::FLAC;
    return CodecID::Unknown;
}

}
