/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CharacterTypes.h>
#include <AK/Format.h>
#include <AK/GenericShorthands.h>
#include <AK/Types.h>

namespace Media::ISOBMFF {

class FourCC {
public:
    constexpr FourCC() = default;

    constexpr explicit FourCC(char const (&characters)[5])
        : m_value((static_cast<u32>(static_cast<u8>(characters[0])) << 24)
              | (static_cast<u32>(static_cast<u8>(characters[1])) << 16)
              | (static_cast<u32>(static_cast<u8>(characters[2])) << 8)
              | static_cast<u32>(static_cast<u8>(characters[3])))
    {
    }

    constexpr explicit FourCC(u32 value)
        : m_value(value)
    {
    }

    constexpr u32 value() const { return m_value; }

    constexpr bool operator==(FourCC const&) const = default;

private:
    u32 m_value { 0 };
};

constexpr FourCC FILE_TYPE_BOX { "ftyp" };
constexpr FourCC SEGMENT_TYPE_BOX { "styp" };
constexpr FourCC MEDIA_DATA_BOX { "mdat" };
constexpr FourCC FREE_SPACE_BOX { "free" };
constexpr FourCC SKIP_BOX { "skip" };
constexpr FourCC EXTENDED_TYPE_BOX { "uuid" };
constexpr FourCC PROGRESSIVE_DOWNLOAD_INFO_BOX { "pdin" };
constexpr FourCC SEGMENT_INDEX_BOX { "sidx" };
constexpr FourCC SUBSEGMENT_INDEX_BOX { "ssix" };
constexpr FourCC PRODUCER_REFERENCE_TIME_BOX { "prft" };
constexpr FourCC META_BOX { "meta" };
constexpr FourCC ADDITIONAL_METADATA_CONTAINER_BOX { "meco" };
constexpr FourCC MOVIE_FRAGMENT_RANDOM_ACCESS_BOX { "mfra" };
constexpr FourCC EVENT_MESSAGE_BOX { "emsg" };
constexpr FourCC BASE_LOCATION_BOX { "bloc" };

constexpr FourCC MOVIE_BOX { "moov" };
constexpr FourCC MOVIE_HEADER_BOX { "mvhd" };
constexpr FourCC TRACK_BOX { "trak" };
constexpr FourCC TRACK_HEADER_BOX { "tkhd" };
constexpr FourCC EDIT_BOX { "edts" };
constexpr FourCC EDIT_LIST_BOX { "elst" };
constexpr FourCC MEDIA_BOX { "mdia" };
constexpr FourCC MEDIA_HEADER_BOX { "mdhd" };
constexpr FourCC HANDLER_BOX { "hdlr" };
constexpr FourCC MEDIA_INFORMATION_BOX { "minf" };
constexpr FourCC DATA_INFORMATION_BOX { "dinf" };
constexpr FourCC DATA_REFERENCE_BOX { "dref" };
constexpr FourCC SAMPLE_TABLE_BOX { "stbl" };
constexpr FourCC SAMPLE_DESCRIPTION_BOX { "stsd" };
constexpr FourCC TIME_TO_SAMPLE_BOX { "stts" };
constexpr FourCC SAMPLE_TO_CHUNK_BOX { "stsc" };
constexpr FourCC SAMPLE_SIZE_BOX { "stsz" };
constexpr FourCC CHUNK_OFFSET_BOX { "stco" };
constexpr FourCC CHUNK_LARGE_OFFSET_BOX { "co64" };

constexpr FourCC MOVIE_EXTENDS_BOX { "mvex" };
constexpr FourCC MOVIE_EXTENDS_HEADER_BOX { "mehd" };
constexpr FourCC TRACK_EXTENDS_BOX { "trex" };

constexpr FourCC MOVIE_FRAGMENT_BOX { "moof" };
constexpr FourCC MOVIE_FRAGMENT_HEADER_BOX { "mfhd" };
constexpr FourCC TRACK_FRAGMENT_BOX { "traf" };
constexpr FourCC TRACK_FRAGMENT_HEADER_BOX { "tfhd" };
constexpr FourCC TRACK_FRAGMENT_DECODE_TIME_BOX { "tfdt" };
constexpr FourCC TRACK_RUN_BOX { "trun" };

constexpr bool is_top_level_box(FourCC type)
{
    return first_is_one_of(type,
        FILE_TYPE_BOX,
        SEGMENT_TYPE_BOX,
        MEDIA_DATA_BOX,
        FREE_SPACE_BOX,
        SKIP_BOX,
        EXTENDED_TYPE_BOX,
        PROGRESSIVE_DOWNLOAD_INFO_BOX,
        SEGMENT_INDEX_BOX,
        SUBSEGMENT_INDEX_BOX,
        PRODUCER_REFERENCE_TIME_BOX,
        META_BOX,
        ADDITIONAL_METADATA_CONTAINER_BOX,
        MOVIE_FRAGMENT_RANDOM_ACCESS_BOX,
        EVENT_MESSAGE_BOX,
        BASE_LOCATION_BOX,
        MOVIE_BOX,
        MOVIE_FRAGMENT_BOX);
}

constexpr FourCC VIDEO_HANDLER { "vide" };
constexpr FourCC AUDIO_HANDLER { "soun" };
constexpr FourCC TEXT_HANDLER { "text" };
constexpr FourCC SUBTITLE_HANDLER { "subt" };
constexpr FourCC METADATA_HANDLER { "meta" };

constexpr FourCC AV1_SAMPLE_ENTRY { "av01" };
constexpr FourCC AV1_CONFIGURATION_BOX { "av1C" };
constexpr FourCC AVC_SAMPLE_ENTRY { "avc1" };
constexpr FourCC AVC_INBAND_PARAMETERS_SAMPLE_ENTRY { "avc3" };
constexpr FourCC AVC_CONFIGURATION_BOX { "avcC" };
constexpr FourCC HEVC_SAMPLE_ENTRY { "hvc1" };
constexpr FourCC HEVC_INBAND_PARAMETERS_SAMPLE_ENTRY { "hev1" };
constexpr FourCC HEVC_CONFIGURATION_BOX { "hvcC" };
constexpr FourCC VP8_SAMPLE_ENTRY { "vp08" };
constexpr FourCC VP9_SAMPLE_ENTRY { "vp09" };
constexpr FourCC VP_CONFIGURATION_BOX { "vpcC" };
constexpr FourCC MPEG4_AUDIO_SAMPLE_ENTRY { "mp4a" };
constexpr FourCC ELEMENTARY_STREAM_DESCRIPTOR_BOX { "esds" };
constexpr FourCC OPUS_SAMPLE_ENTRY { "Opus" };
constexpr FourCC OPUS_CONFIGURATION_BOX { "dOps" };
constexpr FourCC FLAC_SAMPLE_ENTRY { "fLaC" };
constexpr FourCC FLAC_CONFIGURATION_BOX { "dfLa" };

constexpr FourCC COLOUR_INFORMATION_BOX { "colr" };
constexpr FourCC NCLX_COLOUR_TYPE { "nclx" };

}

namespace AK {

template<>
struct Formatter<Media::ISOBMFF::FourCC> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::ISOBMFF::FourCC four_cc)
    {
        char characters[4];
        for (size_t i = 0; i < 4; i++) {
            auto character = static_cast<char>((four_cc.value() >> ((3 - i) * 8)) & 0xFF);
            characters[i] = is_ascii_printable(character) ? character : '.';
        }
        return Formatter<StringView>::format(builder, StringView { characters, 4 });
    }
};

}
