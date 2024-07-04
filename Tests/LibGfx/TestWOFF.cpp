/*
 * Copyright (c) 2023, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/WOFF/Font.h>
#include <LibTest/TestCase.h>

#define TEST_INPUT(x) ("test-inputs/" x)

TEST_CASE(malformed_woff)
{
    Array test_inputs = {
        TEST_INPUT("woff/invalid_sfnt_size.woff"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = MUST(Core::MappedFile::map(test_input));
        auto font_or_error = WOFF::Font::try_load_from_externally_owned_memory(file->bytes());
        EXPECT(font_or_error.is_error());
    }
}
