/*
 * Copyright (c) 2023, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/WOFF2/Font.h>
#include <LibTest/TestCase.h>

#define TEST_INPUT(x) ("test-inputs/" x)

TEST_CASE(tolerate_incorrect_sfnt_size)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("woff2/incorrect_sfnt_size.woff2"sv)));
    auto font = TRY_OR_FAIL(WOFF2::Font::try_load_from_externally_owned_memory(file->bytes()));
    EXPECT_EQ(font->family(), "Test"_string);
    EXPECT_EQ(font->glyph_count(), 4u);
}

TEST_CASE(malformed_woff2)
{
    Array test_inputs = {
        TEST_INPUT("woff2/incorrect_compressed_size.woff2"sv),
        TEST_INPUT("woff2/invalid_numtables.woff2"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = MUST(Core::MappedFile::map(test_input));
        auto font_or_error = WOFF2::Font::try_load_from_externally_owned_memory(file->bytes());
        EXPECT(font_or_error.is_error());
    }
}
