/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>

namespace Unicode {

bool may_require_bidi_processing(Utf16View const&);

}

struct UnicodeBidiParagraph {
    u8 base_level;
    // Whether first-strong resolution found a strong character outside isolates.
    bool has_strong_direction;
};

extern "C" {
// Returns length if the text contains no character with bidi class B.
size_t unicode_bidi_find_paragraph_separator(u16 const* text, size_t length);
// The high bit of each level marks characters that reset under L1. A bidi paragraph separator is encoded as 0xFF.
UnicodeBidiParagraph unicode_bidi_resolve_paragraph(u16 const* text, i32 length, u8 base_level, bool first_strong, u8* levels);
// Returns the start of the suffix whose levels reset to the paragraph level.
i32 unicode_bidi_reorder_line(u8 const* paragraph_levels, i32 length, u8 base_level, i32* indices);
}
