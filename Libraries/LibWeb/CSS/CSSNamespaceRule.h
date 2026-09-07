/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class CSSNamespaceRule final : public CSSRule {
    WEB_WRAPPABLE(CSSNamespaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNamespaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSNamespaceRule> create(RustRule);

    virtual ~CSSNamespaceRule() = default;

    Utf16View namespace_uri() const;
    Utf16View prefix() const;

private:
    explicit CSSNamespaceRule(RustRule);

    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Parser::ValueParserFFI::NamespaceRuleData const& m_rule;
};

}
