/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Object.h>
#include <LibLocale/PluralRules.h>

namespace JS::Intl {

class PluralRules final : public NumberFormatBase {
    JS_OBJECT(PluralRules, NumberFormatBase);
    JS_DECLARE_ALLOCATOR(PluralRules);

public:
    virtual ~PluralRules() override = default;

    ::Locale::PluralForm type() const { return m_type; }
    StringView type_string() const { return ::Locale::plural_form_to_string(m_type); }
    void set_type(StringView type) { m_type = ::Locale::plural_form_from_string(type); }

private:
    explicit PluralRules(Object& prototype);

    ::Locale::PluralForm m_type { ::Locale::PluralForm::Cardinal }; // [[Type]]
};

struct ResolvedPlurality {
    ::Locale::PluralCategory plural_category; // [[PluralCategory]]
    String formatted_string;                  // [[FormattedString]]
};

::Locale::PluralOperands get_operands(StringView string);
::Locale::PluralCategory plural_rule_select(StringView locale, ::Locale::PluralForm type, Value number, ::Locale::PluralOperands operands);
ResolvedPlurality resolve_plural(PluralRules const&, Value number);
::Locale::PluralCategory plural_rule_select_range(StringView locale, ::Locale::PluralForm, ::Locale::PluralCategory start, ::Locale::PluralCategory end);
ThrowCompletionOr<::Locale::PluralCategory> resolve_plural_range(VM&, PluralRules const&, Value start, Value end);

}
