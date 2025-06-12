/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Collator.h>
#include <LibUnicode/ICU.h>

#include <unicode/coll.h>

namespace Unicode {

Usage usage_from_string(StringView usage)
{
    if (usage == "sort"sv)
        return Usage::Sort;
    if (usage == "search"sv)
        return Usage::Search;
    VERIFY_NOT_REACHED();
}

StringView usage_to_string(Usage usage)
{
    switch (usage) {
    case Usage::Sort:
        return "sort"sv;
    case Usage::Search:
        return "search"sv;
    }
    VERIFY_NOT_REACHED();
}

static NonnullOwnPtr<icu::Locale> apply_usage_to_locale(icu::Locale const& locale, Usage usage, StringView collation)
{
    auto result = adopt_own(*locale.clone());
    UErrorCode status = U_ZERO_ERROR;

    switch (usage) {
    case Usage::Sort:
        result->setUnicodeKeywordValue("co", icu_string_piece(collation), status);
        break;
    case Usage::Search:
        result->setUnicodeKeywordValue("co", "search", status);
        break;
    }

    VERIFY(icu_success(status));
    return result;
}

Sensitivity sensitivity_from_string(StringView sensitivity)
{
    if (sensitivity == "base"sv)
        return Sensitivity::Base;
    if (sensitivity == "accent"sv)
        return Sensitivity::Accent;
    if (sensitivity == "case"sv)
        return Sensitivity::Case;
    if (sensitivity == "variant"sv)
        return Sensitivity::Variant;
    VERIFY_NOT_REACHED();
}

StringView sensitivity_to_string(Sensitivity sensitivity)
{
    switch (sensitivity) {
    case Sensitivity::Base:
        return "base"sv;
    case Sensitivity::Accent:
        return "accent"sv;
    case Sensitivity::Case:
        return "case"sv;
    case Sensitivity::Variant:
        return "variant"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UColAttributeValue icu_sensitivity(Sensitivity sensitivity)
{
    switch (sensitivity) {
    case Sensitivity::Base:
        return UCOL_PRIMARY;
    case Sensitivity::Accent:
        return UCOL_SECONDARY;
    case Sensitivity::Case:
        return UCOL_PRIMARY;
    case Sensitivity::Variant:
        return UCOL_TERTIARY;
    }
    VERIFY_NOT_REACHED();
}

static Sensitivity sensitivity_for_collator(icu::Collator const& collator)
{
    UErrorCode status = U_ZERO_ERROR;

    auto attribute = collator.getAttribute(UCOL_STRENGTH, status);
    VERIFY(icu_success(status));

    switch (attribute) {
    case UCOL_PRIMARY:
        attribute = collator.getAttribute(UCOL_CASE_LEVEL, status);
        VERIFY(icu_success(status));

        return attribute == UCOL_ON ? Sensitivity::Case : Sensitivity::Base;

    case UCOL_SECONDARY:
        return Sensitivity::Accent;

    default:
        return Sensitivity::Variant;
    }
}

CaseFirst case_first_from_string(StringView case_first)
{
    if (case_first == "upper"sv)
        return CaseFirst::Upper;
    if (case_first == "lower"sv)
        return CaseFirst::Lower;
    if (case_first == "false"sv)
        return CaseFirst::False;
    VERIFY_NOT_REACHED();
}

StringView case_first_to_string(CaseFirst case_first)
{
    switch (case_first) {
    case CaseFirst::Upper:
        return "upper"sv;
    case CaseFirst::Lower:
        return "lower"sv;
    case CaseFirst::False:
        return "false"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UColAttributeValue icu_case_first(CaseFirst case_first)
{
    switch (case_first) {
    case CaseFirst::Upper:
        return UCOL_UPPER_FIRST;
    case CaseFirst::Lower:
        return UCOL_LOWER_FIRST;
    case CaseFirst::False:
        return UCOL_OFF;
    }
    VERIFY_NOT_REACHED();
}

static bool ignore_punctuation_for_collator(icu::Collator const& collator)
{
    UErrorCode status = U_ZERO_ERROR;

    auto attribute = collator.getAttribute(UCOL_ALTERNATE_HANDLING, status);
    VERIFY(icu_success(status));

    return attribute == UCOL_SHIFTED;
}

class CollatorImpl : public Collator {
public:
    explicit CollatorImpl(NonnullOwnPtr<icu::Collator> collator)
        : m_collator(move(collator))
    {
    }

    virtual Collator::Order compare(StringView lhs, StringView rhs) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto result = m_collator->compareUTF8(icu_string_piece(lhs), icu_string_piece(rhs), status);
        VERIFY(icu_success(status));

        switch (result) {
        case UCOL_LESS:
            return Order::Before;
        case UCOL_EQUAL:
            return Order::Equal;
        case UCOL_GREATER:
            return Order::After;
        }

        VERIFY_NOT_REACHED();
    }

    virtual Sensitivity sensitivity() const override
    {
        return sensitivity_for_collator(*m_collator);
    }

    virtual bool ignore_punctuation() const override
    {
        return ignore_punctuation_for_collator(*m_collator);
    }

private:
    NonnullOwnPtr<icu::Collator> m_collator;
};

NonnullOwnPtr<Collator> Collator::create(
    StringView locale,
    Usage usage,
    StringView collation,
    Optional<Sensitivity> sensitivity,
    CaseFirst case_first,
    bool numeric,
    Optional<bool> ignore_punctuation)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    VERIFY(locale_data.has_value());

    auto locale_with_usage = apply_usage_to_locale(locale_data->locale(), usage, collation);

    auto collator = adopt_own(*icu::Collator::createInstance(*locale_with_usage, status));
    VERIFY(icu_success(status));

    auto set_attribute = [&](UColAttribute attribute, UColAttributeValue value) {
        collator->setAttribute(attribute, value, status);
        VERIFY(icu_success(status));
    };

    if (!sensitivity.has_value())
        sensitivity = sensitivity_for_collator(*collator);

    if (!ignore_punctuation.has_value())
        ignore_punctuation = ignore_punctuation_for_collator(*collator);

    set_attribute(UCOL_STRENGTH, icu_sensitivity(*sensitivity));
    set_attribute(UCOL_CASE_LEVEL, sensitivity == Sensitivity::Case ? UCOL_ON : UCOL_OFF);
    set_attribute(UCOL_CASE_FIRST, icu_case_first(case_first));
    set_attribute(UCOL_NUMERIC_COLLATION, numeric ? UCOL_ON : UCOL_OFF);
    set_attribute(UCOL_ALTERNATE_HANDLING, *ignore_punctuation ? UCOL_SHIFTED : UCOL_NON_IGNORABLE);
    set_attribute(UCOL_NORMALIZATION_MODE, UCOL_ON);

    return adopt_own(*new CollatorImpl(move(collator)));
}

}
