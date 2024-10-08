/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/Find.h>
#include <AK/Traits.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/ICU.h>

#include <unicode/uchar.h>
#include <unicode/uscript.h>

namespace Unicode {

template<typename PropertyType>
struct PropertyName {
    Optional<StringView> long_name;
    Optional<StringView> short_name;
    Optional<StringView> additional_name;
};

// From uchar.h:
// Unicode allows for additional names, beyond the long and short name, which would be indicated by U_LONG_PROPERTY_NAME + i
static constexpr auto ADDITIONAL_NAME = static_cast<UPropertyNameChoice>(U_LONG_PROPERTY_NAME + 1);

}

template<typename PropertyType>
struct AK::Traits<Unicode::PropertyName<PropertyType>> {
    static constexpr bool equals(Unicode::PropertyName<PropertyType> const& candidate, StringView property)
    {
        return property == candidate.long_name || property == candidate.short_name || property == candidate.additional_name;
    }
};

namespace Unicode {

static constexpr GeneralCategory GENERAL_CATEGORY_CASED_LETTER = U_CHAR_CATEGORY_COUNT + 1;
static constexpr GeneralCategory GENERAL_CATEGORY_LETTER = U_CHAR_CATEGORY_COUNT + 2;
static constexpr GeneralCategory GENERAL_CATEGORY_MARK = U_CHAR_CATEGORY_COUNT + 3;
static constexpr GeneralCategory GENERAL_CATEGORY_NUMBER = U_CHAR_CATEGORY_COUNT + 4;
static constexpr GeneralCategory GENERAL_CATEGORY_PUNCTUATION = U_CHAR_CATEGORY_COUNT + 5;
static constexpr GeneralCategory GENERAL_CATEGORY_SYMBOL = U_CHAR_CATEGORY_COUNT + 6;
static constexpr GeneralCategory GENERAL_CATEGORY_SEPARATOR = U_CHAR_CATEGORY_COUNT + 7;
static constexpr GeneralCategory GENERAL_CATEGORY_OTHER = U_CHAR_CATEGORY_COUNT + 8;
static constexpr GeneralCategory GENERAL_CATEGORY_LIMIT = U_CHAR_CATEGORY_COUNT + 9;

Optional<GeneralCategory> general_category_from_string(StringView general_category)
{
    static auto general_category_names = []() {
        Array<PropertyName<GeneralCategory>, GENERAL_CATEGORY_LIMIT.value()> names;

        auto set_names = [&](auto property, auto index, auto general_category) {
            if (char const* name = u_getPropertyValueName(property, general_category, U_LONG_PROPERTY_NAME))
                names[index.value()].long_name = StringView { name, strlen(name) };
            if (char const* name = u_getPropertyValueName(property, general_category, U_SHORT_PROPERTY_NAME))
                names[index.value()].short_name = StringView { name, strlen(name) };
            if (char const* name = u_getPropertyValueName(property, general_category, ADDITIONAL_NAME))
                names[index.value()].additional_name = StringView { name, strlen(name) };
        };

        for (GeneralCategory general_category = 0; general_category < U_CHAR_CATEGORY_COUNT; ++general_category)
            set_names(UCHAR_GENERAL_CATEGORY, general_category, static_cast<UCharCategory>(general_category.value()));

        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_CASED_LETTER, U_GC_LC_MASK);
        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_LETTER, U_GC_L_MASK);
        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_MARK, U_GC_M_MASK);
        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_NUMBER, U_GC_N_MASK);
        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_PUNCTUATION, U_GC_P_MASK);
        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_SYMBOL, U_GC_S_MASK);
        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_SEPARATOR, U_GC_Z_MASK);
        set_names(UCHAR_GENERAL_CATEGORY_MASK, GENERAL_CATEGORY_OTHER, U_GC_C_MASK);

        return names;
    }();

    if (auto index = find_index(general_category_names.begin(), general_category_names.end(), general_category); index != general_category_names.size())
        return static_cast<GeneralCategory>(index);
    return {};
}

bool code_point_has_general_category(u32 code_point, GeneralCategory general_category)
{
    auto icu_code_point = static_cast<UChar32>(code_point);
    auto icu_general_category = static_cast<UCharCategory>(general_category.value());

    if (general_category == GENERAL_CATEGORY_CASED_LETTER)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_LC_MASK) != 0;
    if (general_category == GENERAL_CATEGORY_LETTER)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_L_MASK) != 0;
    if (general_category == GENERAL_CATEGORY_MARK)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_M_MASK) != 0;
    if (general_category == GENERAL_CATEGORY_NUMBER)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_N_MASK) != 0;
    if (general_category == GENERAL_CATEGORY_PUNCTUATION)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_P_MASK) != 0;
    if (general_category == GENERAL_CATEGORY_SYMBOL)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_S_MASK) != 0;
    if (general_category == GENERAL_CATEGORY_SEPARATOR)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_Z_MASK) != 0;
    if (general_category == GENERAL_CATEGORY_OTHER)
        return (U_GET_GC_MASK(icu_code_point) & U_GC_C_MASK) != 0;

    return u_charType(icu_code_point) == icu_general_category;
}

bool code_point_has_control_general_category(u32 code_point)
{
    return code_point_has_general_category(code_point, U_CONTROL_CHAR);
}

bool code_point_has_letter_general_category(u32 code_point)
{
    return code_point_has_general_category(code_point, GENERAL_CATEGORY_LETTER);
}

bool code_point_has_number_general_category(u32 code_point)
{
    return code_point_has_general_category(code_point, GENERAL_CATEGORY_NUMBER);
}

bool code_point_has_punctuation_general_category(u32 code_point)
{
    return code_point_has_general_category(code_point, GENERAL_CATEGORY_PUNCTUATION);
}

bool code_point_has_separator_general_category(u32 code_point)
{
    return code_point_has_general_category(code_point, GENERAL_CATEGORY_SEPARATOR);
}

bool code_point_has_space_separator_general_category(u32 code_point)
{
    return code_point_has_general_category(code_point, U_SPACE_SEPARATOR);
}

bool code_point_has_symbol_general_category(u32 code_point)
{
    return code_point_has_general_category(code_point, GENERAL_CATEGORY_SYMBOL);
}

static constexpr Property PROPERTY_ANY = UCHAR_BINARY_LIMIT + 1;
static constexpr Property PROPERTY_ASCII = UCHAR_BINARY_LIMIT + 2;
static constexpr Property PROPERTY_ASSIGNED = UCHAR_BINARY_LIMIT + 3;
static constexpr Property PROPERTY_LIMIT = UCHAR_BINARY_LIMIT + 4;

Optional<Property> property_from_string(StringView property)
{
    static auto property_names = []() {
        Array<PropertyName<Property>, PROPERTY_LIMIT.value()> names;

        for (Property property = 0; property < UCHAR_BINARY_LIMIT; ++property) {
            auto icu_property = static_cast<UProperty>(property.value());

            if (char const* name = u_getPropertyName(icu_property, U_LONG_PROPERTY_NAME))
                names[property.value()].long_name = StringView { name, strlen(name) };
            if (char const* name = u_getPropertyName(icu_property, U_SHORT_PROPERTY_NAME))
                names[property.value()].short_name = StringView { name, strlen(name) };
            if (char const* name = u_getPropertyName(icu_property, ADDITIONAL_NAME))
                names[property.value()].additional_name = StringView { name, strlen(name) };
        }

        names[PROPERTY_ANY.value()] = { "Any"sv, {}, {} };
        names[PROPERTY_ASCII.value()] = { "ASCII"sv, {}, {} };
        names[PROPERTY_ASSIGNED.value()] = { "Assigned"sv, {}, {} };

        return names;
    }();

    if (auto index = find_index(property_names.begin(), property_names.end(), property); index != property_names.size())
        return static_cast<Property>(index);
    return {};
}

bool code_point_has_property(u32 code_point, Property property)
{
    auto icu_code_point = static_cast<UChar32>(code_point);
    auto icu_property = static_cast<UProperty>(property.value());

    if (property == PROPERTY_ANY)
        return is_unicode(code_point);
    if (property == PROPERTY_ASCII)
        return is_ascii(code_point);
    if (property == PROPERTY_ASSIGNED)
        return u_isdefined(icu_code_point);

    return static_cast<bool>(u_hasBinaryProperty(icu_code_point, icu_property));
}

bool code_point_has_emoji_property(u32 code_point)
{
    return code_point_has_property(code_point, UCHAR_EMOJI);
}

bool code_point_has_emoji_modifier_base_property(u32 code_point)
{
    return code_point_has_property(code_point, UCHAR_EMOJI_MODIFIER_BASE);
}

bool code_point_has_emoji_presentation_property(u32 code_point)
{
    return code_point_has_property(code_point, UCHAR_EMOJI_PRESENTATION);
}

bool code_point_has_identifier_start_property(u32 code_point)
{
    return u_isIDStart(static_cast<UChar32>(code_point));
}

bool code_point_has_identifier_continue_property(u32 code_point)
{
    return u_isIDPart(static_cast<UChar32>(code_point));
}

bool code_point_has_regional_indicator_property(u32 code_point)
{
    return code_point_has_property(code_point, UCHAR_REGIONAL_INDICATOR);
}

bool code_point_has_variation_selector_property(u32 code_point)
{
    return code_point_has_property(code_point, UCHAR_VARIATION_SELECTOR);
}

// https://tc39.es/ecma262/#table-binary-unicode-properties
bool is_ecma262_property(Property property)
{
    if (property == PROPERTY_ANY || property == PROPERTY_ASCII || property == PROPERTY_ASSIGNED)
        return true;

    switch (property.value()) {
    case UCHAR_ASCII_HEX_DIGIT:
    case UCHAR_ALPHABETIC:
    case UCHAR_BIDI_CONTROL:
    case UCHAR_BIDI_MIRRORED:
    case UCHAR_CASE_IGNORABLE:
    case UCHAR_CASED:
    case UCHAR_CHANGES_WHEN_CASEFOLDED:
    case UCHAR_CHANGES_WHEN_CASEMAPPED:
    case UCHAR_CHANGES_WHEN_LOWERCASED:
    case UCHAR_CHANGES_WHEN_NFKC_CASEFOLDED:
    case UCHAR_CHANGES_WHEN_TITLECASED:
    case UCHAR_CHANGES_WHEN_UPPERCASED:
    case UCHAR_DASH:
    case UCHAR_DEFAULT_IGNORABLE_CODE_POINT:
    case UCHAR_DEPRECATED:
    case UCHAR_DIACRITIC:
    case UCHAR_EMOJI:
    case UCHAR_EMOJI_COMPONENT:
    case UCHAR_EMOJI_MODIFIER:
    case UCHAR_EMOJI_MODIFIER_BASE:
    case UCHAR_EMOJI_PRESENTATION:
    case UCHAR_EXTENDED_PICTOGRAPHIC:
    case UCHAR_EXTENDER:
    case UCHAR_GRAPHEME_BASE:
    case UCHAR_GRAPHEME_EXTEND:
    case UCHAR_HEX_DIGIT:
    case UCHAR_IDS_BINARY_OPERATOR:
    case UCHAR_IDS_TRINARY_OPERATOR:
    case UCHAR_ID_CONTINUE:
    case UCHAR_ID_START:
    case UCHAR_IDEOGRAPHIC:
    case UCHAR_JOIN_CONTROL:
    case UCHAR_LOGICAL_ORDER_EXCEPTION:
    case UCHAR_LOWERCASE:
    case UCHAR_MATH:
    case UCHAR_NONCHARACTER_CODE_POINT:
    case UCHAR_PATTERN_SYNTAX:
    case UCHAR_PATTERN_WHITE_SPACE:
    case UCHAR_QUOTATION_MARK:
    case UCHAR_RADICAL:
    case UCHAR_REGIONAL_INDICATOR:
    case UCHAR_S_TERM:
    case UCHAR_SOFT_DOTTED:
    case UCHAR_TERMINAL_PUNCTUATION:
    case UCHAR_UNIFIED_IDEOGRAPH:
    case UCHAR_UPPERCASE:
    case UCHAR_VARIATION_SELECTOR:
    case UCHAR_WHITE_SPACE:
    case UCHAR_XID_CONTINUE:
    case UCHAR_XID_START:
        return true;
    default:
        return false;
    }
}

Optional<Script> script_from_string(StringView script)
{
    static auto script_names = []() {
        Array<PropertyName<Script>, static_cast<size_t>(USCRIPT_CODE_LIMIT)> names;

        for (Script script = 0; script < USCRIPT_CODE_LIMIT; ++script) {
            auto icu_script = static_cast<UScriptCode>(script.value());

            if (char const* name = uscript_getName(icu_script))
                names[script.value()].long_name = StringView { name, strlen(name) };
            if (char const* name = uscript_getShortName(icu_script))
                names[script.value()].short_name = StringView { name, strlen(name) };
            if (char const* name = u_getPropertyValueName(UCHAR_SCRIPT, icu_script, ADDITIONAL_NAME))
                names[script.value()].additional_name = StringView { name, strlen(name) };
        }

        return names;
    }();

    if (auto index = find_index(script_names.begin(), script_names.end(), script); index != script_names.size())
        return static_cast<Script>(index);
    return {};
}

bool code_point_has_script(u32 code_point, Script script)
{
    UErrorCode status = U_ZERO_ERROR;

    auto icu_code_point = static_cast<UChar32>(code_point);
    auto icu_script = static_cast<UScriptCode>(script.value());

    if (auto result = uscript_getScript(icu_code_point, &status); icu_success(status))
        return result == icu_script;
    return false;
}

bool code_point_has_script_extension(u32 code_point, Script script)
{
    auto icu_code_point = static_cast<UChar32>(code_point);
    auto icu_script = static_cast<UScriptCode>(script.value());

    return static_cast<bool>(uscript_hasScript(icu_code_point, icu_script));
}

static constexpr BidiClass char_direction_to_bidi_class(UCharDirection direction)
{
    switch (direction) {
    case U_ARABIC_NUMBER:
        return BidiClass::ArabicNumber;
    case U_BLOCK_SEPARATOR:
        return BidiClass::BlockSeparator;
    case U_BOUNDARY_NEUTRAL:
        return BidiClass::BoundaryNeutral;
    case U_COMMON_NUMBER_SEPARATOR:
        return BidiClass::CommonNumberSeparator;
    case U_DIR_NON_SPACING_MARK:
        return BidiClass::DirNonSpacingMark;
    case U_EUROPEAN_NUMBER:
        return BidiClass::EuropeanNumber;
    case U_EUROPEAN_NUMBER_SEPARATOR:
        return BidiClass::EuropeanNumberSeparator;
    case U_EUROPEAN_NUMBER_TERMINATOR:
        return BidiClass::EuropeanNumberTerminator;
    case U_FIRST_STRONG_ISOLATE:
        return BidiClass::FirstStrongIsolate;
    case U_LEFT_TO_RIGHT:
        return BidiClass::LeftToRight;
    case U_LEFT_TO_RIGHT_EMBEDDING:
        return BidiClass::LeftToRightEmbedding;
    case U_LEFT_TO_RIGHT_ISOLATE:
        return BidiClass::LeftToRightIsolate;
    case U_LEFT_TO_RIGHT_OVERRIDE:
        return BidiClass::LeftToRightOverride;
    case U_OTHER_NEUTRAL:
        return BidiClass::OtherNeutral;
    case U_POP_DIRECTIONAL_FORMAT:
        return BidiClass::PopDirectionalFormat;
    case U_POP_DIRECTIONAL_ISOLATE:
        return BidiClass::PopDirectionalIsolate;
    case U_RIGHT_TO_LEFT:
        return BidiClass::RightToLeft;
    case U_RIGHT_TO_LEFT_ARABIC:
        return BidiClass::RightToLeftArabic;
    case U_RIGHT_TO_LEFT_EMBEDDING:
        return BidiClass::RightToLeftEmbedding;
    case U_RIGHT_TO_LEFT_ISOLATE:
        return BidiClass::RightToLeftIsolate;
    case U_RIGHT_TO_LEFT_OVERRIDE:
        return BidiClass::RightToLeftOverride;
    case U_SEGMENT_SEPARATOR:
        return BidiClass::SegmentSeparator;
    case U_WHITE_SPACE_NEUTRAL:
        return BidiClass::WhiteSpaceNeutral;
    case U_CHAR_DIRECTION_COUNT:
        break;
    }
    VERIFY_NOT_REACHED();
}

BidiClass bidirectional_class(u32 code_point)
{
    auto icu_code_point = static_cast<UChar32>(code_point);

    auto direction = u_charDirection(icu_code_point);
    return char_direction_to_bidi_class(direction);
}

}
