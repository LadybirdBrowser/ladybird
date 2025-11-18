/*
 * Copyright (c) 2023, Stephan Vedder <stephan.vedder@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <LibMedia/TrackType.h>

namespace Media {

enum class CodecID : u32 {
    Unknown,
    // On2 / Google
    VP8,
    VP9,
    // MPEG
    H261,
    MPEG1,
    H262,
    H263,
    H264,
    H265,
    MP3,
    AAC,
    // AOMedia
    AV1,
    // Xiph
    Theora,
    Vorbis,
    Opus,
    FLAC,
};

inline TrackType track_type_from_codec_id(CodecID codec)
{
    switch (codec) {
    case CodecID::VP8:
    case CodecID::VP9:
    case CodecID::H261:
    case CodecID::MPEG1:
    case CodecID::H262:
    case CodecID::H263:
    case CodecID::H264:
    case CodecID::H265:
    case CodecID::AV1:
        return TrackType::Video;
    case CodecID::MP3:
    case CodecID::AAC:
    case CodecID::Theora:
    case CodecID::Vorbis:
    case CodecID::Opus:
    case CodecID::FLAC:
        return TrackType::Audio;
    case CodecID::Unknown:
        break;
    }
    return TrackType::Unknown;
}

}

namespace AK {

template<>
struct Formatter<Media::CodecID> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::CodecID value)
    {
        StringView codec;
        switch (value) {
        case Media::CodecID::Unknown:
            codec = "Unknown"sv;
            break;
        case Media::CodecID::VP8:
            codec = "VP8"sv;
            break;
        case Media::CodecID::VP9:
            codec = "VP9"sv;
            break;
        case Media::CodecID::H261:
            codec = "H.261"sv;
            break;
        case Media::CodecID::H262:
            codec = "H.262"sv;
            break;
        case Media::CodecID::H263:
            codec = "H.263"sv;
            break;
        case Media::CodecID::H264:
            codec = "H.264"sv;
            break;
        case Media::CodecID::H265:
            codec = "H.265"sv;
            break;
        case Media::CodecID::MP3:
            codec = "MP3"sv;
            break;
        case Media::CodecID::AAC:
            codec = "AAC"sv;
            break;
        case Media::CodecID::MPEG1:
            codec = "MPEG1"sv;
            break;
        case Media::CodecID::AV1:
            codec = "AV1"sv;
            break;
        case Media::CodecID::Theora:
            codec = "Theora"sv;
            break;
        case Media::CodecID::Vorbis:
            codec = "Vorbis"sv;
            break;
        case Media::CodecID::Opus:
            codec = "Opus"sv;
            break;
        case Media::CodecID::FLAC:
            codec = "FLAC"sv;
            break;
        }
        return builder.put_string(codec);
    }
};

}
