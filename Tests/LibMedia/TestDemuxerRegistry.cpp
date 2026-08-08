/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibTest/TestCase.h>

using Media::CodecID;
using Media::ContainerID;
using Media::ContainerMediaType;
using Media::ContainerMimeType;

TEST_CASE(supported_file_containers)
{
    for (auto container : {
             ContainerMimeType { ContainerID::ISOBMFF, ContainerMediaType::Audio },
             ContainerMimeType { ContainerID::ISOBMFF, ContainerMediaType::Video },
             ContainerMimeType { ContainerID::Matroska, ContainerMediaType::Audio },
             ContainerMimeType { ContainerID::Matroska, ContainerMediaType::Video },
             ContainerMimeType { ContainerID::WebM, ContainerMediaType::Audio },
             ContainerMimeType { ContainerID::WebM, ContainerMediaType::Video },
             ContainerMimeType { ContainerID::Ogg, ContainerMediaType::Audio },
             ContainerMimeType { ContainerID::Ogg, ContainerMediaType::Video },
             ContainerMimeType { ContainerID::Ogg, ContainerMediaType::Application },
             ContainerMimeType { ContainerID::MPEGAudio, ContainerMediaType::Audio },
             ContainerMimeType { ContainerID::FLAC, ContainerMediaType::Audio },
             ContainerMimeType { ContainerID::WAV, ContainerMediaType::Audio },
         }) {
        EXPECT(Media::is_supported_file_container(container));
    }

    EXPECT(!Media::is_supported_file_container({ ContainerID::ISOBMFF, ContainerMediaType::Application }));
    EXPECT(!Media::is_supported_file_container({ ContainerID::Matroska, ContainerMediaType::Application }));
    EXPECT(!Media::is_supported_file_container({ ContainerID::WAV, ContainerMediaType::Video }));
}

TEST_CASE(supported_file_container_codecs)
{
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::ISOBMFF, ContainerMediaType::Video }, CodecID::H264));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::ISOBMFF, ContainerMediaType::Video }, CodecID::AAC));
    EXPECT(!Media::is_codec_supported_in_file_container({ ContainerID::ISOBMFF, ContainerMediaType::Video }, CodecID::Theora));

    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::WebM, ContainerMediaType::Video }, CodecID::VP9));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::WebM, ContainerMediaType::Video }, CodecID::Opus));
    EXPECT(!Media::is_codec_supported_in_file_container({ ContainerID::WebM, ContainerMediaType::Video }, CodecID::H264));
    EXPECT(!Media::is_codec_supported_in_file_container({ ContainerID::WebM, ContainerMediaType::Audio }, CodecID::VP9));

    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::Matroska, ContainerMediaType::Video }, CodecID::H264));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::Matroska, ContainerMediaType::Video }, CodecID::AAC));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::Matroska, ContainerMediaType::Audio }, CodecID::FLAC));

    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::Ogg, ContainerMediaType::Video }, CodecID::Theora));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::Ogg, ContainerMediaType::Audio }, CodecID::Vorbis));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::Ogg, ContainerMediaType::Application }, CodecID::Opus));
    EXPECT(!Media::is_codec_supported_in_file_container({ ContainerID::Ogg, ContainerMediaType::Video }, CodecID::H264));

    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::MPEGAudio, ContainerMediaType::Audio }, CodecID::MP3));
    EXPECT(!Media::is_codec_supported_in_file_container({ ContainerID::MPEGAudio, ContainerMediaType::Audio }, CodecID::AAC));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::FLAC, ContainerMediaType::Audio }, CodecID::FLAC));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::WAV, ContainerMediaType::Audio }, CodecID::S16LE));
    EXPECT(Media::is_codec_supported_in_file_container({ ContainerID::WAV, ContainerMediaType::Audio }, CodecID::MuLaw));
}
