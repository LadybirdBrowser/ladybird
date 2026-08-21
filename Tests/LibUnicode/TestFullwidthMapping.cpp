/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>
#include <LibUnicode/FullwidthMapping.h>

TEST_CASE(fullwidth_mapping_reports_length_changing_graphemes)
{
    auto source = Utf16String::from_utf8("ｶﾞAﾊﾟ"sv);
    auto result = Unicode::apply_fullwidth_mapping(source);

    EXPECT_EQ(result.text, Utf16String::from_utf8("ガＡパ"sv));
    EXPECT_EQ(result.edits.size(), 2u);
    EXPECT_EQ(result.edits[0].source_start, 0u);
    EXPECT_EQ(result.edits[0].source_length, 2u);
    EXPECT_EQ(result.edits[0].destination_start, 0u);
    EXPECT_EQ(result.edits[0].destination_length, 1u);
    EXPECT_EQ(result.edits[1].source_start, 3u);
    EXPECT_EQ(result.edits[1].source_length, 2u);
    EXPECT_EQ(result.edits[1].destination_start, 2u);
    EXPECT_EQ(result.edits[1].destination_length, 1u);
}
