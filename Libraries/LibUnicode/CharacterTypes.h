/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/IterationDecision.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>

namespace Unicode {

Optional<GeneralCategory> general_category_from_string(StringView);
bool code_point_has_general_category(u32 code_point, GeneralCategory general_category, CaseSensitivity case_sensitivity = CaseSensitivity::CaseSensitive);

bool code_point_is_printable(u32 code_point);
bool code_point_has_control_general_category(u32 code_point);
bool code_point_has_letter_general_category(u32 code_point);
bool code_point_has_mark_general_category(u32 code_point);
bool code_point_has_number_general_category(u32 code_point);
bool code_point_has_punctuation_general_category(u32 code_point);
bool code_point_has_separator_general_category(u32 code_point);
bool code_point_has_space_separator_general_category(u32 code_point);
bool code_point_has_symbol_general_category(u32 code_point);

Optional<Property> property_from_string(StringView);
bool code_point_has_property(u32 code_point, Property property, CaseSensitivity case_sensitivity = CaseSensitivity::CaseSensitive);

bool code_point_has_emoji_property(u32 code_point);
bool code_point_has_emoji_modifier_base_property(u32 code_point);
bool code_point_has_emoji_presentation_property(u32 code_point);
bool code_point_has_identifier_start_property(u32 code_point);
bool code_point_has_identifier_continue_property(u32 code_point);
bool code_point_has_regional_indicator_property(u32 code_point);
bool code_point_has_variation_selector_property(u32 code_point);
bool code_point_has_white_space_property(u32 code_point);

bool is_ecma262_property(Property);
bool is_ecma262_string_property(Property);
Vector<String> get_property_strings(Property);

Optional<Script> script_from_string(StringView);
bool code_point_has_script(u32 code_point, Script script);
bool code_point_has_script_extension(u32 code_point, Script script);

enum class BidiClass {
    ArabicNumber,             // AN
    BlockSeparator,           // B
    BoundaryNeutral,          // BN
    CommonNumberSeparator,    // CS
    DirNonSpacingMark,        // NSM
    EuropeanNumber,           // EN
    EuropeanNumberSeparator,  // ES
    EuropeanNumberTerminator, // ET
    FirstStrongIsolate,       // FSI
    LeftToRight,              // L
    LeftToRightEmbedding,     // LRE
    LeftToRightIsolate,       // LRI
    LeftToRightOverride,      // LRO
    OtherNeutral,             // ON
    PopDirectionalFormat,     // PDF
    PopDirectionalIsolate,    // PDI
    RightToLeft,              // R
    RightToLeftArabic,        // AL
    RightToLeftEmbedding,     // RLE
    RightToLeftIsolate,       // RLI
    RightToLeftOverride,      // RLO
    SegmentSeparator,         // S
    WhiteSpaceNeutral,        // WS
};

BidiClass bidirectional_class(u32 code_point);

enum class LineBreakClass {
    Alphabetic,     // AL
    Numeric,        // NU
    Ideographic,    // ID
    Ambiguous,      // AI
    ComplexContext, // SA
    CombiningMark,  // CM
    Other,
};

LineBreakClass line_break_class(u32 code_point);

struct CodePointRange {
    u32 from { 0 };
    u32 to { 0 };
};

u32 canonicalize(u32 code_point, bool unicode_mode);

bool code_point_matches_range_ignoring_case(u32 code_point, u32 from, u32 to, bool unicode_mode);

Vector<CodePointRange> expand_range_case_insensitive(u32 from, u32 to);

void for_each_case_folded_code_point(u32 code_point, Function<IterationDecision(u32)> callback);

template<typename Range1, typename Range2>
bool ranges_equal_ignoring_case(Range1 const& range1, Range2 const& range2, bool unicode_mode)
{
    auto it1 = range1.begin();
    auto it2 = range2.begin();
    auto end1 = range1.end();
    auto end2 = range2.end();

    for (; it1 != end1 && it2 != end2; ++it1, ++it2) {
        if (canonicalize(*it1, unicode_mode) != canonicalize(*it2, unicode_mode))
            return false;
    }

    return it1 == end1 && it2 == end2;
}

}
