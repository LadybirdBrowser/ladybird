/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/DisplayNames.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DisplayNames);

// 12 DisplayNames Objects, https://tc39.es/ecma402/#intl-displaynames-objects
DisplayNames::DisplayNames(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

void DisplayNames::set_type(StringView type)
{
    if (type == "language"sv)
        m_type = Type::Language;
    else if (type == "region"sv)
        m_type = Type::Region;
    else if (type == "script"sv)
        m_type = Type::Script;
    else if (type == "currency"sv)
        m_type = Type::Currency;
    else if (type == "calendar"sv)
        m_type = Type::Calendar;
    else if (type == "dateTimeField"sv)
        m_type = Type::DateTimeField;
    else
        VERIFY_NOT_REACHED();
}

StringView DisplayNames::type_string() const
{
    switch (m_type) {
    case Type::Language:
        return "language"sv;
    case Type::Region:
        return "region"sv;
    case Type::Script:
        return "script"sv;
    case Type::Currency:
        return "currency"sv;
    case Type::Calendar:
        return "calendar"sv;
    case Type::DateTimeField:
        return "dateTimeField"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

void DisplayNames::set_fallback(StringView fallback)
{
    if (fallback == "none"sv)
        m_fallback = Fallback::None;
    else if (fallback == "code"sv)
        m_fallback = Fallback::Code;
    else
        VERIFY_NOT_REACHED();
}

StringView DisplayNames::fallback_string() const
{
    switch (m_fallback) {
    case Fallback::None:
        return "none"sv;
    case Fallback::Code:
        return "code"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

// 12.5.1 CanonicalCodeForDisplayNames ( type, code ), https://tc39.es/ecma402/#sec-canonicalcodefordisplaynames
ThrowCompletionOr<Value> canonical_code_for_display_names(VM& vm, DisplayNames::Type type, StringView code)
{
    // 1. If type is "language", then
    if (type == DisplayNames::Type::Language) {
        // a. If code does not match the unicode_language_id production, throw a RangeError exception.
        if (!Unicode::parse_unicode_language_id(code).has_value())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, code, "language"sv);

        // b. If IsStructurallyValidLanguageTag(code) is false, throw a RangeError exception.
        if (!is_structurally_valid_language_tag(code))
            return vm.throw_completion<RangeError>(ErrorType::IntlInvalidLanguageTag, code);

        // c. Return ! CanonicalizeUnicodeLocaleId(code).
        auto canonicalized_tag = canonicalize_unicode_locale_id(code);
        return PrimitiveString::create(vm, move(canonicalized_tag));
    }

    // 2. If type is "region", then
    if (type == DisplayNames::Type::Region) {
        // a. If code does not match the unicode_region_subtag production, throw a RangeError exception.
        if (!Unicode::is_unicode_region_subtag(code))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, code, "region"sv);

        // b. Return the ASCII-uppercase of code.
        return PrimitiveString::create(vm, code.to_uppercase_string());
    }

    // 3. If type is "script", then
    if (type == DisplayNames::Type::Script) {
        // a. If code does not match the unicode_script_subtag production, throw a RangeError exception.
        if (!Unicode::is_unicode_script_subtag(code))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, code, "script"sv);

        // Assert: The length of code is 4, and every code unit of code represents an ASCII letter (0x0041 through 0x005A and 0x0061 through 0x007A, both inclusive).
        VERIFY(code.length() == 4);
        VERIFY(all_of(code, is_ascii_alpha));

        // c. Let first be the ASCII-uppercase of the substring of code from 0 to 1.
        // d. Let rest be the ASCII-lowercase of the substring of code from 1.
        // e. Return the string-concatenation of first and rest.
        return PrimitiveString::create(vm, code.to_titlecase_string());
    }

    // 4. If type is "calendar", then
    if (type == DisplayNames::Type::Calendar) {
        // a. If code does not match the Unicode Locale Identifier type nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(code))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, code, "calendar"sv);

        // b. If code uses any of the backwards compatibility syntax described in Unicode Technical Standard #35 LDML § 3.3 BCP 47 Conformance, throw a RangeError exception.
        if (code.contains('_'))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, code, "calendar"sv);

        // c. Return the ASCII-lowercase of code.
        return PrimitiveString::create(vm, code.to_lowercase_string());
    }

    // 5. If type is "dateTimeField", then
    if (type == DisplayNames::Type::DateTimeField) {
        // a. If the result of IsValidDateTimeFieldCode(code) is false, throw a RangeError exception.
        if (!is_valid_date_time_field_code(code))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, code, "dateTimeField"sv);

        // b. Return code.
        return PrimitiveString::create(vm, code);
    }

    // 6. Assert: type is "currency".
    VERIFY(type == DisplayNames::Type::Currency);

    // 7. If ! IsWellFormedCurrencyCode(code) is false, throw a RangeError exception.
    if (!is_well_formed_currency_code(code))
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, code, "currency"sv);

    // 8. Return the ASCII-uppercase of code.
    return PrimitiveString::create(vm, code.to_uppercase_string());
}

// 12.5.2 IsValidDateTimeFieldCode ( field ), https://tc39.es/ecma402/#sec-isvaliddatetimefieldcode
bool is_valid_date_time_field_code(StringView field)
{
    // 1. If field is listed in the Code column of Table 19, return true.
    // 2. Return false.
    return field.is_one_of("era"sv, "year"sv, "quarter"sv, "month"sv, "weekOfYear"sv, "weekday"sv, "day"sv, "dayPeriod"sv, "hour"sv, "minute"sv, "second"sv, "timeZoneName"sv);
}

}
