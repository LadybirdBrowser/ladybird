/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Color/ColorMetadata.h>
#include <LibTest/TestCase.h>

TEST_CASE(adopt_specified_values_ignores_reserved_and_unspecified_values)
{
    Media::CodingIndependentCodePoints cicp {
        Media::ColorPrimaries::BT709, Media::TransferCharacteristics::BT709,
        Media::MatrixCoefficients::BT709, Media::VideoFullRangeFlag::Studio
    };

    cicp.adopt_specified_values({ static_cast<Media::ColorPrimaries>(0), static_cast<Media::TransferCharacteristics>(0),
        Media::MatrixCoefficients::Unspecified, Media::VideoFullRangeFlag::Unspecified });

    EXPECT_EQ(cicp.color_primaries(), Media::ColorPrimaries::BT709);
    EXPECT_EQ(cicp.transfer_characteristics(), Media::TransferCharacteristics::BT709);
    EXPECT_EQ(cicp.matrix_coefficients(), Media::MatrixCoefficients::BT709);
    EXPECT_EQ(cicp.video_full_range_flag(), Media::VideoFullRangeFlag::Studio);
}

TEST_CASE(adopt_specified_values_adopts_valid_values)
{
    Media::CodingIndependentCodePoints cicp {
        Media::ColorPrimaries::Unspecified, Media::TransferCharacteristics::Unspecified,
        Media::MatrixCoefficients::Unspecified, Media::VideoFullRangeFlag::Unspecified
    };

    cicp.adopt_specified_values({ Media::ColorPrimaries::BT2020, Media::TransferCharacteristics::SRGB,
        Media::MatrixCoefficients::BT601, Media::VideoFullRangeFlag::Full });

    EXPECT_EQ(cicp.color_primaries(), Media::ColorPrimaries::BT2020);
    EXPECT_EQ(cicp.transfer_characteristics(), Media::TransferCharacteristics::SRGB);
    EXPECT_EQ(cicp.matrix_coefficients(), Media::MatrixCoefficients::BT601);
    EXPECT_EQ(cicp.video_full_range_flag(), Media::VideoFullRangeFlag::Full);
}

TEST_CASE(adopting_content_light_levels_keeps_known_levels_over_unknown_ones)
{
    Media::ColorMetadata metadata;
    metadata.content_light_level_information = Media::ContentLightLevelInformation { .maximum_content_light_level = 1000, .maximum_frame_average_light_level = 400 };

    Media::ColorMetadata unknown_peak;
    unknown_peak.content_light_level_information = Media::ContentLightLevelInformation { .maximum_content_light_level = 0, .maximum_frame_average_light_level = 300 };
    metadata.adopt_specified_values(unknown_peak);
    EXPECT_EQ(metadata.content_light_level_information->maximum_content_light_level, 1000);
    EXPECT_EQ(metadata.content_light_level_information->maximum_frame_average_light_level, 300);

    Media::ColorMetadata unknown_levels;
    unknown_levels.content_light_level_information = Media::ContentLightLevelInformation {};
    metadata.adopt_specified_values(unknown_levels);
    EXPECT_EQ(metadata.content_light_level_information->maximum_content_light_level, 1000);
    EXPECT_EQ(metadata.content_light_level_information->maximum_frame_average_light_level, 300);
}
