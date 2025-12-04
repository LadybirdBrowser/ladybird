/*
 * Copyright (c) 2024, Alex Ungurianu <alex@ungurianu.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-properties-values-api/#the-css-property-rule-interface
class CSSPropertyRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSPropertyRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSPropertyRule);

public:
    static GC::Ref<CSSPropertyRule> create(JS::Realm&, FlyString name, FlyString syntax, bool inherits, RefPtr<StyleValue const> initial_value);

    virtual ~CSSPropertyRule() = default;

    FlyString const& name() const { return m_name; }
    FlyString const& syntax() const { return m_syntax; }
    bool inherits() const { return m_inherits; }
    Optional<String> initial_value() const;
    RefPtr<StyleValue const> initial_style_value() const { return m_initial_value; }

private:
    CSSPropertyRule(JS::Realm&, FlyString name, FlyString syntax, bool inherits, RefPtr<StyleValue const> initial_value);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    FlyString m_name;
    FlyString m_syntax;
    bool m_inherits;
    RefPtr<StyleValue const> m_initial_value;
};

template<>
inline bool CSSRule::fast_is<CSSPropertyRule>() const { return type() == CSSRule::Type::Property; }

}
