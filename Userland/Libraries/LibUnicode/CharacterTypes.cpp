/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/CharacterTypes.h>

#if ENABLE_UNICODE_DATA
#    include <LibUnicode/UnicodeData.h>
#endif

namespace Unicode {

Optional<GeneralCategory> __attribute__((weak)) general_category_from_string(StringView) { return {}; }
bool __attribute__((weak)) code_point_has_general_category(u32, GeneralCategory) { return {}; }
Optional<Property> __attribute__((weak)) property_from_string(StringView) { return {}; }
bool __attribute__((weak)) code_point_has_property(u32, Property) { return {}; }

bool is_ecma262_property([[maybe_unused]] Property property)
{
#if ENABLE_UNICODE_DATA
    // EMCA-262 only allows a subset of Unicode properties: https://tc39.es/ecma262/#table-binary-unicode-properties
    switch (property) {
    case Unicode::Property::ASCII:
    case Unicode::Property::ASCII_Hex_Digit:
    case Unicode::Property::Alphabetic:
    case Unicode::Property::Any:
    case Unicode::Property::Assigned:
    case Unicode::Property::Bidi_Control:
    case Unicode::Property::Bidi_Mirrored:
    case Unicode::Property::Case_Ignorable:
    case Unicode::Property::Cased:
    case Unicode::Property::Changes_When_Casefolded:
    case Unicode::Property::Changes_When_Casemapped:
    case Unicode::Property::Changes_When_Lowercased:
    case Unicode::Property::Changes_When_NFKC_Casefolded:
    case Unicode::Property::Changes_When_Titlecased:
    case Unicode::Property::Changes_When_Uppercased:
    case Unicode::Property::Dash:
    case Unicode::Property::Default_Ignorable_Code_Point:
    case Unicode::Property::Deprecated:
    case Unicode::Property::Diacritic:
    case Unicode::Property::Emoji:
    case Unicode::Property::Emoji_Component:
    case Unicode::Property::Emoji_Modifier:
    case Unicode::Property::Emoji_Modifier_Base:
    case Unicode::Property::Emoji_Presentation:
    case Unicode::Property::Extended_Pictographic:
    case Unicode::Property::Extender:
    case Unicode::Property::Grapheme_Base:
    case Unicode::Property::Grapheme_Extend:
    case Unicode::Property::Hex_Digit:
    case Unicode::Property::IDS_Binary_Operator:
    case Unicode::Property::IDS_Trinary_Operator:
    case Unicode::Property::ID_Continue:
    case Unicode::Property::ID_Start:
    case Unicode::Property::Ideographic:
    case Unicode::Property::Join_Control:
    case Unicode::Property::Logical_Order_Exception:
    case Unicode::Property::Lowercase:
    case Unicode::Property::Math:
    case Unicode::Property::Noncharacter_Code_Point:
    case Unicode::Property::Pattern_Syntax:
    case Unicode::Property::Pattern_White_Space:
    case Unicode::Property::Quotation_Mark:
    case Unicode::Property::Radical:
    case Unicode::Property::Regional_Indicator:
    case Unicode::Property::Sentence_Terminal:
    case Unicode::Property::Soft_Dotted:
    case Unicode::Property::Terminal_Punctuation:
    case Unicode::Property::Unified_Ideograph:
    case Unicode::Property::Uppercase:
    case Unicode::Property::Variation_Selector:
    case Unicode::Property::White_Space:
    case Unicode::Property::XID_Continue:
    case Unicode::Property::XID_Start:
        return true;
    default:
        return false;
    }
#else
    return false;
#endif
}

Optional<Script> __attribute__((weak)) script_from_string(StringView) { return {}; }
bool __attribute__((weak)) code_point_has_script(u32, Script) { return {}; }
bool __attribute__((weak)) code_point_has_script_extension(u32, Script) { return {}; }

Optional<BidirectionalClass> __attribute__((weak)) bidirectional_class_from_string(StringView) { return {}; }
Optional<BidirectionalClass> __attribute__((weak)) bidirectional_class(u32) { return {}; }

}
