/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#cssmarginrule
class CSSMarginRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSMarginRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSMarginRule);

public:
    [[nodiscard]] static GC::Ref<CSSMarginRule> create(JS::Realm&, Utf16FlyString name, GC::Ref<CSSStyleProperties>);

    virtual ~CSSMarginRule() override = default;

    Utf16FlyString const& name() const { return m_name; }
    GC::Ref<CSSStyleProperties> style() { return m_style; }
    GC::Ref<CSSStyleProperties const> style() const { return m_style; }

private:
    CSSMarginRule(JS::Realm&, Utf16FlyString name, GC::Ref<CSSStyleProperties>);

    virtual void initialize(JS::Realm&) override;
    virtual Utf16String serialized() const override;
    virtual void visit_edges(Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Utf16FlyString m_name;
    GC::Ref<CSSStyleProperties> m_style;
};

bool is_margin_rule_name(Utf16View);

}
