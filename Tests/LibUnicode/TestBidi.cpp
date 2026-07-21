/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>
#include <LibUnicode/Bidi.h>

TEST_CASE(detect_bidirectional_text)
{
    EXPECT(!Unicode::may_require_bidi_processing("Michelangelo’s"_utf16));
    EXPECT(Unicode::may_require_bidi_processing("abc אבג"_utf16));
    EXPECT(Unicode::may_require_bidi_processing("abc\u202Edef"_utf16));
}
