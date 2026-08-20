/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>

namespace Unicode {

enum class CaseMapping {
    Lowercase,
    Uppercase,
    Titlecase,
};

// All offsets and lengths are measured in UTF-16 code units, matching the values reported by ICU.
struct CaseMappingEdit {
    size_t source_start { 0 };
    size_t source_length { 0 };
    size_t destination_start { 0 };
    size_t destination_length { 0 };
};

struct CaseMappingResult {
    Utf16String text;
    Vector<CaseMappingEdit> edits;
};

CaseMappingResult apply_case_mapping(Utf16String const&, CaseMapping, Optional<Utf16View> const& locale = {}, TrailingCodePointTransformation = TrailingCodePointTransformation::Lowercase);

}
