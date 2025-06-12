/*
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/EnumBits.h>

enum class VideoIntro : u8 {
    None = 0x0,
    Well = 0x1,
    Hello = 0x2,
    Friends = 0x4,
    ExclimationMark = 0x8,
    CompleteIntro = Well | Hello | Friends | ExclimationMark,
};

AK_ENUM_BITWISE_OPERATORS(VideoIntro);

TEST_CASE(bitwise_or)
{
    auto intro = VideoIntro::Well | VideoIntro::Hello | VideoIntro::Friends | VideoIntro::ExclimationMark;
    EXPECT_EQ(intro, VideoIntro::CompleteIntro);
}

TEST_CASE(bitwise_and)
{
    auto intro = VideoIntro::CompleteIntro;
    EXPECT_EQ(intro & VideoIntro::Hello, VideoIntro::Hello);
}

TEST_CASE(bitwise_xor)
{
    auto intro = VideoIntro::Well | VideoIntro::Hello | VideoIntro::Friends;
    EXPECT_EQ(intro ^ VideoIntro::CompleteIntro, VideoIntro::ExclimationMark);
}

TEST_CASE(bitwise_not)
{
    auto intro = ~VideoIntro::CompleteIntro;
    EXPECT_EQ(intro & VideoIntro::CompleteIntro, VideoIntro::None);
}

TEST_CASE(bitwise_or_equal)
{
    auto intro = VideoIntro::Well | VideoIntro::Hello | VideoIntro::Friends;
    EXPECT_EQ(intro |= VideoIntro::ExclimationMark, VideoIntro::CompleteIntro);
}

TEST_CASE(bitwise_and_equal)
{
    auto intro = VideoIntro::CompleteIntro;
    EXPECT_EQ(intro &= VideoIntro::Hello, VideoIntro::Hello);
}

TEST_CASE(bitwise_xor_equal)
{
    auto intro = VideoIntro::Well | VideoIntro::Hello | VideoIntro::Friends;
    EXPECT_EQ(intro ^= VideoIntro::CompleteIntro, VideoIntro::ExclimationMark);
}

TEST_CASE(has_flag)
{
    auto intro = VideoIntro::Hello | VideoIntro::Friends;
    EXPECT(has_flag(intro, VideoIntro::Friends));
    EXPECT(!has_flag(intro, VideoIntro::Well));
    EXPECT(!has_flag(intro, VideoIntro::CompleteIntro));
}

TEST_CASE(has_any_flag)
{
    auto intro = VideoIntro::Hello | VideoIntro::Friends;
    EXPECT(has_any_flag(intro, VideoIntro::Friends));
    EXPECT(!has_any_flag(intro, VideoIntro::Well));
    EXPECT(has_any_flag(intro, VideoIntro::CompleteIntro));
}
