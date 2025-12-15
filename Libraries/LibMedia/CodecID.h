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

constexpr StringView codec_id_to_string(CodecID codec)
{
    switch (codec) {
    case Media::CodecID::Unknown:
        return "Unknown"sv;
    case Media::CodecID::VP8:
        return "VP8"sv;
    case Media::CodecID::VP9:
        return "VP9"sv;
    case Media::CodecID::H261:
        return "H.261"sv;
    case Media::CodecID::H262:
        return "H.262"sv;
    case Media::CodecID::H263:
        return "H.263"sv;
    case Media::CodecID::H264:
        return "H.264"sv;
    case Media::CodecID::H265:
        return "H.265"sv;
    case Media::CodecID::MP3:
        return "MP3"sv;
    case Media::CodecID::AAC:
        return "AAC"sv;
    case Media::CodecID::MPEG1:
        return "MPEG1"sv;
    case Media::CodecID::AV1:
        return "AV1"sv;
    case Media::CodecID::Theora:
        return "Theora"sv;
    case Media::CodecID::Vorbis:
        return "Vorbis"sv;
    case Media::CodecID::Opus:
        return "Opus"sv;
    case Media::CodecID::FLAC:
        return "FLAC"sv;
    }
    return "Unknown"sv;
}

}

namespace AK {

template<>
struct Formatter<Media::CodecID> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::CodecID value)
    {
        return Formatter<StringView>::format(builder, Media::codec_id_to_string(value));
    }
};

}
