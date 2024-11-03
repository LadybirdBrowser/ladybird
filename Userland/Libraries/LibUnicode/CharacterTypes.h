/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibUnicode/Forward.h>

namespace Unicode {

Optional<GeneralCategory> general_category_from_string(StringView);
bool code_point_has_general_category(u32 code_point, GeneralCategory general_category);

bool code_point_is_printable(u32 code_point);
bool code_point_has_control_general_category(u32 code_point);
bool code_point_has_letter_general_category(u32 code_point);
bool code_point_has_number_general_category(u32 code_point);
bool code_point_has_punctuation_general_category(u32 code_point);
bool code_point_has_separator_general_category(u32 code_point);
bool code_point_has_space_separator_general_category(u32 code_point);
bool code_point_has_symbol_general_category(u32 code_point);

Optional<Property> property_from_string(StringView);
bool code_point_has_property(u32 code_point, Property property);

bool code_point_has_emoji_property(u32 code_point);
bool code_point_has_emoji_modifier_base_property(u32 code_point);
bool code_point_has_emoji_presentation_property(u32 code_point);
bool code_point_has_identifier_start_property(u32 code_point);
bool code_point_has_identifier_continue_property(u32 code_point);
bool code_point_has_regional_indicator_property(u32 code_point);
bool code_point_has_variation_selector_property(u32 code_point);

bool is_ecma262_property(Property);

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

}
