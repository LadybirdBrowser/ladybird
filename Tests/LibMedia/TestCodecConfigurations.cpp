/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Codecs/Opus.h>
#include <LibTest/TestCase.h>

TEST_CASE(opus_isobmff_configuration_is_normalized_to_opus_head)
{
    Array<u8, 15> configuration {
        0, 2, 0x01, 0x02, 0x00, 0x00, 0xbb, 0x80, 0xff, 0x00, 1, 1, 1, 0, 1
    };
    Array<u8, 23> expected {
        'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
        1, 2, 0x02, 0x01, 0x80, 0xbb, 0x00, 0x00, 0x00, 0xff, 1, 1, 1, 0, 1
    };

    auto initialization_data = MUST(Media::Codecs::Opus::codec_initialization_data_from_isobmff_configuration(configuration));
    EXPECT(initialization_data.span() == expected.span());
}

TEST_CASE(opus_isobmff_configuration_rejects_invalid_wrapper)
{
    Array<u8, 11> configuration { 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    auto unsupported_version = configuration;
    unsupported_version[0] = 1;
    EXPECT(Media::Codecs::Opus::codec_initialization_data_from_isobmff_configuration(unsupported_version).is_error());
    EXPECT(Media::Codecs::Opus::codec_initialization_data_from_isobmff_configuration(configuration.span().trim(10)).is_error());
}

TEST_CASE(flac_isobmff_configuration_is_normalized_to_native_metadata)
{
    Array<u8, 49> configuration {
        0, 0, 0, 0,
        0x00, 0x00, 0x00, 0x22,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
        0x84, 0x00, 0x00, 0x03,
        'a', 'b', 'c'
    };

    auto initialization_data = MUST(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(configuration));
    EXPECT(initialization_data.span().slice(0, 4) == "fLaC"sv.bytes());
    EXPECT(initialization_data.span().slice(4) == configuration.span().slice(4));
}

TEST_CASE(flac_isobmff_configuration_rejects_invalid_wrapper)
{
    Array<u8, 42> configuration {
        0, 0, 0, 0,
        0x80, 0x00, 0x00, 0x22,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33
    };

    auto unsupported_version = configuration;
    unsupported_version[0] = 1;
    EXPECT(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(unsupported_version).is_error());

    auto unsupported_flags = configuration;
    unsupported_flags[3] = 1;
    EXPECT(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(unsupported_flags).is_error());
    EXPECT(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(configuration.span().trim(3)).is_error());
}
