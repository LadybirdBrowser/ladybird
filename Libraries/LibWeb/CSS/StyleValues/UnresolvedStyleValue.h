/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/SubstitutionFunctionsPresence.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API UnresolvedStyleValue final : public StyleValue {
public:
    enum class SourceTextMode : u8 {
        Trim,
        TrimLeading,
        Preserve,
    };

    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create(Utf16String token_source, Parser::SubstitutionFunctionsPresence, SourceTextMode = SourceTextMode::Trim, bool contains_attr_tainted_values = false);
    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create_attr_tainted_with_parsed_value(Utf16String token_source, Parser::SubstitutionFunctionsPresence, SourceTextMode, NonnullRefPtr<StyleValue const> parsed_value);
    virtual ~UnresolvedStyleValue() override = default;

    bool contains_attr_tainted_values() const { return m_value->unresolved.contains_attr_tainted_values; }
    RefPtr<StyleValue const> parsed_value() const { return wrap_rust_child_or_null(m_value->unresolved.parsed_value); }

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;

private:
    friend class StyleValue;

    explicit UnresolvedStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValue(Type::Unresolved, data)
    {
    }

    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create_internal(Utf16String token_source, Parser::SubstitutionFunctionsPresence, SourceTextMode, bool contains_attr_tainted_values, RefPtr<StyleValue const> parsed_value);
};

}
