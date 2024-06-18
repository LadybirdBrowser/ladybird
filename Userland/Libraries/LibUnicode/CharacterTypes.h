/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>

namespace Unicode {

struct CodePointRange {
    u32 first { 0 };
    u32 last { 0 };
};

struct CodePointRangeComparator {
    constexpr int operator()(u32 code_point, CodePointRange const& range)
    {
        return (code_point > range.last) - (code_point < range.first);
    }
};

u32 canonical_combining_class(u32 code_point);

// Note: The single code point case conversions only perform simple case folding.
// Use the full-string transformations for full case folding.
u32 to_unicode_lowercase(u32 code_point);
u32 to_unicode_uppercase(u32 code_point);
u32 to_unicode_titlecase(u32 code_point);

template<typename ViewType>
bool equals_ignoring_case(ViewType, ViewType);

template<typename ViewType>
Optional<size_t> find_ignoring_case(ViewType, ViewType);

Optional<GeneralCategory> general_category_from_string(StringView);
bool code_point_has_general_category(u32 code_point, GeneralCategory general_category);

Optional<Property> property_from_string(StringView);
bool code_point_has_property(u32 code_point, Property property);
bool is_ecma262_property(Property);

Optional<Script> script_from_string(StringView);
bool code_point_has_script(u32 code_point, Script script);
bool code_point_has_script_extension(u32 code_point, Script script);

bool code_point_has_grapheme_break_property(u32 code_point, GraphemeBreakProperty property);
bool code_point_has_word_break_property(u32 code_point, WordBreakProperty property);
bool code_point_has_sentence_break_property(u32 code_point, SentenceBreakProperty property);

Optional<BidirectionalClass> bidirectional_class_from_string(StringView);
Optional<BidirectionalClass> bidirectional_class(u32 code_point);

}
