/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

::Locale::PluralCategory resolve_plural(PluralRules const&, Value number);
ThrowCompletionOr<::Locale::PluralCategory> resolve_plural_range(VM&, PluralRules const&, Value start, Value end);

}
