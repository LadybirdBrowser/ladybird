/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Unicode {

enum class CaseMapping : u8 {
    Lowercase,
    Uppercase,
    Titlecase,
};

}

// The caller owns the destination and edit storage. All offsets are UTF-16 code units.
struct UnicodeTextMappingOutput {
    void* context;
    u16* (*allocate_text)(void*, size_t);
    void (*append_edit)(void*, size_t source_start, size_t source_length, size_t destination_start, size_t destination_length);
};

extern "C" {
void unicode_apply_case_mapping(u16 const* text, size_t length, u8 mapping, u16 const* locale, size_t locale_length, bool preserve_existing, UnicodeTextMappingOutput);
void unicode_apply_fullwidth_mapping(u16 const* text, size_t length, UnicodeTextMappingOutput);
bool unicode_text_may_require_bidi_processing(u16 const* text, size_t length);
}
