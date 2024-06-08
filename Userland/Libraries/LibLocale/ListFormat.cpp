/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/NonnullOwnPtr.h>
#include <LibLocale/ICU.h>
#include <LibLocale/ListFormat.h>

#include <unicode/listformatter.h>

namespace Locale {

ListFormatType list_format_type_from_string(StringView list_format_type)
{
    if (list_format_type == "conjunction"sv)
        return ListFormatType::Conjunction;
    if (list_format_type == "disjunction"sv)
        return ListFormatType::Disjunction;
    if (list_format_type == "unit"sv)
        return ListFormatType::Unit;
    VERIFY_NOT_REACHED();
}

StringView list_format_type_to_string(ListFormatType list_format_type)
{
    switch (list_format_type) {
    case ListFormatType::Conjunction:
        return "conjunction"sv;
    case ListFormatType::Disjunction:
        return "disjunction"sv;
    case ListFormatType::Unit:
        return "unit"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

static constexpr UListFormatterType icu_list_format_type(ListFormatType type)
{
    switch (type) {
    case ListFormatType::Conjunction:
        return ULISTFMT_TYPE_AND;
    case ListFormatType::Disjunction:
        return ULISTFMT_TYPE_OR;
    case ListFormatType::Unit:
        return ULISTFMT_TYPE_UNITS;
    }

    VERIFY_NOT_REACHED();
}

static constexpr UListFormatterWidth icu_list_format_width(Style style)
{
    switch (style) {
    case Style::Long:
        return ULISTFMT_WIDTH_WIDE;
    case Style::Short:
        return ULISTFMT_WIDTH_SHORT;
    case Style::Narrow:
        return ULISTFMT_WIDTH_NARROW;
    }

    VERIFY_NOT_REACHED();
}

static constexpr StringView icu_list_format_field_to_string(i32 field)
{
    switch (field) {
    case ULISTFMT_LITERAL_FIELD:
        return "literal"sv;
    case ULISTFMT_ELEMENT_FIELD:
        return "element"sv;
    }

    VERIFY_NOT_REACHED();
}

struct FormatResult {
    icu::FormattedList list;
    icu::UnicodeString string;
};

static Optional<FormatResult> format_list_impl(StringView locale, ListFormatType type, Style style, ReadonlySpan<String> list)
{
    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    UErrorCode status = U_ZERO_ERROR;

    auto list_formatter = adopt_own(*icu::ListFormatter::createInstance(locale_data->locale(), icu_list_format_type(type), icu_list_format_width(style), status));
    if (icu_failure(status))
        return {};

    auto icu_list = icu_string_list(list);

    auto formatted_list = list_formatter->formatStringsToValue(icu_list.data(), static_cast<i32>(icu_list.size()), status);
    if (icu_failure(status))
        return {};

    auto formatted_string = formatted_list.toString(status);
    if (icu_failure(status))
        return {};

    return FormatResult { move(formatted_list), move(formatted_string) };
}

String format_list(StringView locale, ListFormatType type, Style style, ReadonlySpan<String> list)
{
    auto formatted = format_list_impl(locale, type, style, list);
    if (!formatted.has_value())
        return {};

    return icu_string_to_string(formatted->string);
}

Vector<ListFormatPart> format_list_to_parts(StringView locale, ListFormatType type, Style style, ReadonlySpan<String> list)
{
    UErrorCode status = U_ZERO_ERROR;

    auto formatted = format_list_impl(locale, type, style, list);
    if (!formatted.has_value())
        return {};

    Vector<ListFormatPart> result;

    icu::ConstrainedFieldPosition position;
    position.constrainCategory(UFIELD_CATEGORY_LIST);

    while (static_cast<bool>(formatted->list.nextPosition(position, status)) && icu_success(status)) {
        auto type = icu_list_format_field_to_string(position.getField());
        auto part = formatted->string.tempSubStringBetween(position.getStart(), position.getLimit());

        result.empend(type, icu_string_to_string(part));
    }

    return result;
}

}
