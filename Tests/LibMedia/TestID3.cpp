/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/Containers/ID3.h>
#include <LibTest/TestCase.h>

static Array<u8, 10> version_2_header(u8 version, u8 flags, Array<u8, 4> size)
{
    return { 'I', 'D', '3', version, 0, flags, size[0], size[1], size[2], size[3] };
}

TEST_CASE(version_2_tag_size_covers_the_header_and_its_contents)
{
    auto header = version_2_header(3, 0, { 0, 0, 2, 1 });
    EXPECT_EQ(Media::ID3::version_2_tag_size(header), 10u + 257u);
}

TEST_CASE(version_2_tag_size_includes_the_footer_it_declares)
{
    auto with_footer = version_2_header(4, 0x10, { 0, 0, 0, 20 });
    EXPECT_EQ(Media::ID3::version_2_tag_size(with_footer), 10u + 20u + 10u);

    // The footer only exists from version 4 onwards, so the same flag means nothing before it.
    auto without_footer = version_2_header(3, 0x10, { 0, 0, 0, 20 });
    EXPECT_EQ(Media::ID3::version_2_tag_size(without_footer), 10u + 20u);
}

TEST_CASE(version_2_tag_size_rejects_bytes_that_only_look_like_a_tag)
{
    Array<u8, 10> audio { 0xFF, 0xF1, 0x4C, 0xA0, 0x01, 0xA0, 0x00, 0x21, 0x20, 0x03 };
    EXPECT(!Media::ID3::version_2_tag_size(audio).has_value());

    auto unknown_version = version_2_header(0xFF, 0, { 0, 0, 0, 20 });
    EXPECT(!Media::ID3::version_2_tag_size(unknown_version).has_value());

    // A size byte may not use its most significant bit.
    auto unsynchronized_size = version_2_header(4, 0, { 0, 0, 0x80, 20 });
    EXPECT(!Media::ID3::version_2_tag_size(unsynchronized_size).has_value());

    auto truncated = version_2_header(4, 0, { 0, 0, 0, 20 });
    EXPECT(!Media::ID3::version_2_tag_size(truncated.span().trim(9)).has_value());
}
