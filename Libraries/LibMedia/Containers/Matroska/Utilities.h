/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/CodecID.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Containers/Matroska/Document.h>

namespace Media::Matroska {

inline CodecID codec_id_from_matroska_track_entry(TrackEntry const& track)
{
    auto codec_id = track.codec_id();
    if (codec_id == "V_VP8")
        return CodecID::VP8;
    if (codec_id == "V_VP9")
        return CodecID::VP9;
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

    auto audio_track = track.audio_track();
    if (!audio_track.has_value())
        return CodecID::Unknown;

    auto bit_depth = audio_track->bit_depth;
    if (codec_id == "A_PCM/FLOAT/IEEE")
        return bit_depth == 32 ? CodecID::F32LE : CodecID::Unknown;

    if (codec_id == "A_PCM/INT/BIG")
        return bit_depth == 8 ? CodecID::U8 : CodecID::Unknown;

    if (codec_id == "A_PCM/INT/LIT") {
        switch (bit_depth) {
        case 8:
            return CodecID::U8;
        case 16:
            return CodecID::S16LE;
        case 24:
            return CodecID::S24LE;
        case 32:
            return CodecID::S32LE;
        default:
            return CodecID::Unknown;
        }
    }

    return CodecID::Unknown;
}

constexpr bool supports_codec_in_container(ContainerID container_id, CodecID codec_id)
{
    if (container_id == ContainerID::WebM) {
        switch (codec_id) {
        case CodecID::VP8:
        case CodecID::VP9:
        case CodecID::AV1:
        case CodecID::Vorbis:
        case CodecID::Opus:
            return true;
        default:
            return false;
        }
    }

    if (container_id != ContainerID::Matroska)
        return false;

    switch (codec_id) {
    case CodecID::VP8:
    case CodecID::VP9:
    case CodecID::H264:
    case CodecID::H265:
    case CodecID::MP3:
    case CodecID::AAC:
    case CodecID::AV1:
    case CodecID::Theora:
    case CodecID::Vorbis:
    case CodecID::Opus:
    case CodecID::FLAC:
    case CodecID::U8:
    case CodecID::S16LE:
    case CodecID::S24LE:
    case CodecID::S32LE:
    case CodecID::F32LE:
        return true;
    default:
        return false;
    }
}

}
