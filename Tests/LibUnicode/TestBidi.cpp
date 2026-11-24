/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>
#include <LibUnicode/Bidi.h>

TEST_CASE(detect_bidirectional_text)
{
    EXPECT(!Unicode::may_require_bidi_processing("Michelangelo’s"_utf16));
    EXPECT(Unicode::may_require_bidi_processing("abc אבג"_utf16));
    EXPECT(Unicode::may_require_bidi_processing("abc\u202Edef"_utf16));
    EXPECT(Unicode::may_require_bidi_processing("abc \u0661\u0662 \u0663\u0664"_utf16));
    EXPECT(!Unicode::may_require_bidi_processing("abc 12 34"_utf16));
}

TEST_CASE(find_bidi_paragraph_separator)
{
    // A supplementary character takes two code units. U+2028 is a line separator with bidi class WS.
    constexpr Array<u16, 5> text { 0xd83d, 0xde00, 0x2028, 0x2029, 'a' };
    EXPECT_EQ(unicode_bidi_find_paragraph_separator(text.data(), text.size()), 3u);
    EXPECT_EQ(unicode_bidi_find_paragraph_separator(text.data(), 3), 3u);
}

TEST_CASE(reset_trailing_bidi_levels_after_wrapping)
{
    constexpr Array<u16, 4> text { 'a', 0x05d0, ' ', 0x05d1 };
    Array<u8, 4> levels {};
    auto paragraph = unicode_bidi_resolve_paragraph(text.data(), text.size(), 0, false, levels.data());
    Array<i32, 3> indices {};
    // The space between the Hebrew letters resolves as RTL. Wrapping after it resets it to LTR.
    EXPECT_EQ(unicode_bidi_reorder_line(levels.data(), indices.size(), paragraph.base_level, indices.data()), 2);
    EXPECT_EQ(indices[0], 0);
    EXPECT_EQ(indices[1], 1);
    EXPECT_EQ(indices[2], 2);
}

TEST_CASE(distinguish_first_strong_direction_from_default_level)
{
    Array<u16, 5> text { 0x2067, 0x05d0, 0x2069, '1', '?' };
    Array<u8, 5> levels {};
    auto paragraph = unicode_bidi_resolve_paragraph(text.data(), text.size(), 1, true, levels.data());
    EXPECT_EQ(paragraph.base_level, 0u);
    EXPECT(!paragraph.has_strong_direction);

    text[4] = 'A';
    paragraph = unicode_bidi_resolve_paragraph(text.data(), text.size(), 1, true, levels.data());
    EXPECT_EQ(paragraph.base_level, 0u);
    EXPECT(paragraph.has_strong_direction);

    text[4] = 0x05d0;
    paragraph = unicode_bidi_resolve_paragraph(text.data(), text.size(), 1, true, levels.data());
    EXPECT_EQ(paragraph.base_level, 1u);
    EXPECT(paragraph.has_strong_direction);
}
