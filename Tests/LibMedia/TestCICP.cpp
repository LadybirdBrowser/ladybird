/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibTest/TestCase.h>

TEST_CASE(adopt_specified_values_ignores_reserved_and_unspecified_values)
{
    Media::CodingIndependentCodePoints cicp {
        Media::ColorPrimaries::BT709, Media::TransferCharacteristics::BT709,
        Media::MatrixCoefficients::BT709, Media::VideoFullRangeFlag::Studio
    };

    cicp.adopt_specified_values({ Media::ColorPrimaries::Reserved, Media::TransferCharacteristics::Reserved,
        Media::MatrixCoefficients::Unspecified, Media::VideoFullRangeFlag::Unspecified });

    EXPECT(cicp.color_primaries() == Media::ColorPrimaries::BT709);
    EXPECT(cicp.transfer_characteristics() == Media::TransferCharacteristics::BT709);
    EXPECT(cicp.matrix_coefficients() == Media::MatrixCoefficients::BT709);
    EXPECT(cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Studio);
}

TEST_CASE(adopt_specified_values_adopts_valid_values)
{
    Media::CodingIndependentCodePoints cicp {
        Media::ColorPrimaries::Unspecified, Media::TransferCharacteristics::Unspecified,
        Media::MatrixCoefficients::Unspecified, Media::VideoFullRangeFlag::Unspecified
    };

    cicp.adopt_specified_values({ Media::ColorPrimaries::BT2020, Media::TransferCharacteristics::SRGB,
        Media::MatrixCoefficients::BT601, Media::VideoFullRangeFlag::Full });

    EXPECT(cicp.color_primaries() == Media::ColorPrimaries::BT2020);
    EXPECT(cicp.transfer_characteristics() == Media::TransferCharacteristics::SRGB);
    EXPECT(cicp.matrix_coefficients() == Media::MatrixCoefficients::BT601);
    EXPECT(cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full);
}
