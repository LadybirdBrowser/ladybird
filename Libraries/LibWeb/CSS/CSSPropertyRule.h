/*
 * Copyright (c) 2024, Alex Ungurianu <alex@ungurianu.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-properties-values-api/#the-css-property-rule-interface
class CSSPropertyRule final : public CSSRule {
    WEB_WRAPPABLE(CSSPropertyRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSPropertyRule);

public:
    static GC::Ref<CSSPropertyRule> create(RustRule);

    virtual ~CSSPropertyRule();

    Utf16View name() const;
    Utf16View syntax() const;
    bool inherits() const;
    Optional<Utf16String> initial_value() const;

private:
    explicit CSSPropertyRule(RustRule);

    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    RefPtr<StyleValue const> initial_style_value() const;
    Parser::ValueParserFFI::PropertyRuleData const& m_rule;
};

template<>
inline bool CSSRule::fast_is<CSSPropertyRule>() const { return type() == CSSRule::Type::Property; }

}
