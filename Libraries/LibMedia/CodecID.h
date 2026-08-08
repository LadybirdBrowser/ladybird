/*
 * Copyright (c) 2023, Stephan Vedder <stephan.vedder@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StringView.h>
#include <LibMedia/TrackType.h>

namespace Media {

enum class CodecID : u8 {
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
    // PCM
    U8,
    S16LE,
    S24LE,
    S32LE,
    S64LE,
    F32LE,
    F64LE,
    ALaw,
    MuLaw,
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
    case CodecID::Theora:
        return TrackType::Video;
    case CodecID::MP3:
    case CodecID::AAC:
    case CodecID::Vorbis:
    case CodecID::Opus:
    case CodecID::FLAC:
    case CodecID::U8:
    case CodecID::S16LE:
    case CodecID::S24LE:
    case CodecID::S32LE:
    case CodecID::S64LE:
    case CodecID::F32LE:
    case CodecID::F64LE:
    case CodecID::ALaw:
    case CodecID::MuLaw:
        return TrackType::Audio;
    case CodecID::Unknown:
        break;
    }
    return TrackType::Unknown;
}

// Maps a codec ID string, as used in the codecs parameter of a MIME type, to a CodecID.  Returns CodecID::Unknown for
// codec ID strings that aren't recognized.
// https://www.rfc-editor.org/rfc/rfc6381
inline CodecID codec_id_from_rfc6381_codec_string(StringView codec_string)
{
    // Codec ID strings for codec families such as AVC, HEVC, VP9, AV1 and AAC have period-separated suffixes that
    // describe the profile, level, and other parameters of the codec used.
    auto is_codec_family = [&](StringView family) {
        if (!codec_string.starts_with(family))
            return false;
        return codec_string.length() == family.length() || codec_string[family.length()] == '.';
    };

    if (is_codec_family("avc1"sv) || is_codec_family("avc3"sv))
        return CodecID::H264;
    if (is_codec_family("hvc1"sv) || is_codec_family("hev1"sv))
        return CodecID::H265;
    if (codec_string == "vp8"sv || is_codec_family("vp08"sv))
        return CodecID::VP8;
    if (codec_string == "vp9"sv || is_codec_family("vp09"sv))
        return CodecID::VP9;
    if (is_codec_family("av01"sv))
        return CodecID::AV1;
    if (codec_string == "theora"sv)
        return CodecID::Theora;
    if (codec_string == "vorbis"sv)
        return CodecID::Vorbis;
    if (codec_string == "opus"sv)
        return CodecID::Opus;
    if (codec_string == "flac"sv)
        return CodecID::FLAC;
    if (codec_string == "mp3"sv)
        return CodecID::MP3;
    // MPEG-4 audio object type 0x40 is MPEG-4 AAC and object types 0x66 to 0x68 are MPEG-2 AAC, while object types 0x69
    // and 0x6B are MPEG-1/2 audio, whose ubiquitous layer is MP3.
    if (is_codec_family("mp4a.40"sv) || codec_string == "mp4a.66"sv || codec_string == "mp4a.67"sv || codec_string == "mp4a.68"sv)
        return CodecID::AAC;
    if (codec_string == "mp4a.69"sv || codec_string == "mp4a.6B"sv || codec_string == "mp4a.6b"sv)
        return CodecID::MP3;
    return CodecID::Unknown;
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
    case CodecID::U8:
        return "unsigned 8-bit PCM"sv;
    case CodecID::S16LE:
        return "signed 16-bit PCM"sv;
    case CodecID::S24LE:
        return "signed 24-bit PCM"sv;
    case CodecID::S32LE:
        return "signed 32-bit PCM"sv;
    case CodecID::S64LE:
        return "signed 64-bit PCM"sv;
    case CodecID::F32LE:
        return "32-bit float PCM"sv;
    case CodecID::F64LE:
        return "64-bit float PCM"sv;
    case CodecID::ALaw:
        return "A-law PCM"sv;
    case CodecID::MuLaw:
        return "μ-law PCM"sv;
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
