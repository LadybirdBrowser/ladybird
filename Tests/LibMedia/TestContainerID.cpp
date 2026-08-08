/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/ContainerID.h>
#include <LibTest/TestCase.h>

TEST_CASE(container_mime_types)
{
    struct TestCase {
        StringView type;
        StringView subtype;
        Media::ContainerID container_id;
        Media::ContainerMediaType media_type;
    };

    for (auto test_case : {
             TestCase { "audio"sv, "mp4"sv, Media::ContainerID::ISOBMFF, Media::ContainerMediaType::Audio },
             TestCase { "video"sv, "mp4"sv, Media::ContainerID::ISOBMFF, Media::ContainerMediaType::Video },
             TestCase { "application"sv, "mp4"sv, Media::ContainerID::ISOBMFF, Media::ContainerMediaType::Application },
             TestCase { "audio"sv, "webm"sv, Media::ContainerID::WebM, Media::ContainerMediaType::Audio },
             TestCase { "video"sv, "webm"sv, Media::ContainerID::WebM, Media::ContainerMediaType::Video },
             TestCase { "audio"sv, "matroska"sv, Media::ContainerID::Matroska, Media::ContainerMediaType::Audio },
             TestCase { "video"sv, "x-matroska"sv, Media::ContainerID::Matroska, Media::ContainerMediaType::Video },
             TestCase { "audio"sv, "ogg"sv, Media::ContainerID::Ogg, Media::ContainerMediaType::Audio },
             TestCase { "video"sv, "ogg"sv, Media::ContainerID::Ogg, Media::ContainerMediaType::Video },
             TestCase { "application"sv, "ogg"sv, Media::ContainerID::Ogg, Media::ContainerMediaType::Application },
             TestCase { "audio"sv, "mpeg"sv, Media::ContainerID::MPEGAudio, Media::ContainerMediaType::Audio },
             TestCase { "audio"sv, "mp3"sv, Media::ContainerID::MPEGAudio, Media::ContainerMediaType::Audio },
             TestCase { "audio"sv, "x-mp3"sv, Media::ContainerID::MPEGAudio, Media::ContainerMediaType::Audio },
             TestCase { "audio"sv, "aac"sv, Media::ContainerID::ADTS, Media::ContainerMediaType::Audio },
             TestCase { "audio"sv, "flac"sv, Media::ContainerID::FLAC, Media::ContainerMediaType::Audio },
             TestCase { "audio"sv, "wav"sv, Media::ContainerID::WAV, Media::ContainerMediaType::Audio },
             TestCase { "audio"sv, "wave"sv, Media::ContainerID::WAV, Media::ContainerMediaType::Audio },
             TestCase { "audio"sv, "x-wav"sv, Media::ContainerID::WAV, Media::ContainerMediaType::Audio },
         }) {
        auto mime_type = Media::container_mime_type_from_mime_type(test_case.type, test_case.subtype);
        EXPECT(mime_type.has_value());
        EXPECT_EQ(mime_type->container_id, test_case.container_id);
        EXPECT_EQ(mime_type->media_type, test_case.media_type);
    }
}

TEST_CASE(invalid_container_mime_types)
{
    EXPECT(!Media::container_mime_type_from_mime_type("image"sv, "webm"sv).has_value());
    EXPECT(!Media::container_mime_type_from_mime_type("application"sv, "webm"sv).has_value());
    EXPECT(!Media::container_mime_type_from_mime_type("application"sv, "matroska"sv).has_value());
    EXPECT(!Media::container_mime_type_from_mime_type("video"sv, "mpeg"sv).has_value());
    EXPECT(!Media::container_mime_type_from_mime_type("video"sv, "wav"sv).has_value());
    EXPECT(!Media::container_mime_type_from_mime_type("audio"sv, "unknown"sv).has_value());
}
