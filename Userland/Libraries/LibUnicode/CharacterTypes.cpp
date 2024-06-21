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

#include <unicode/uchar.h>

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

Optional<GeneralCategory> __attribute__((weak)) general_category_from_string(StringView) { return {}; }
bool __attribute__((weak)) code_point_has_general_category(u32, GeneralCategory) { return {}; }

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

Optional<Script> __attribute__((weak)) script_from_string(StringView) { return {}; }
bool __attribute__((weak)) code_point_has_script(u32, Script) { return {}; }
bool __attribute__((weak)) code_point_has_script_extension(u32, Script) { return {}; }

Optional<BidirectionalClass> __attribute__((weak)) bidirectional_class_from_string(StringView) { return {}; }
Optional<BidirectionalClass> __attribute__((weak)) bidirectional_class(u32) { return {}; }

}
