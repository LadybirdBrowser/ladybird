/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#cssmarginrule
class CSSMarginRule final : public CSSRule {
    WEB_WRAPPABLE(CSSMarginRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSMarginRule);

public:
    [[nodiscard]] static GC::Ref<CSSMarginRule> create(RustRule);

    virtual ~CSSMarginRule() override = default;

    Utf16FlyString const& name() const { return m_name; }
    GC::Ref<CSSStyleProperties> style() const;

private:
    CSSMarginRule(RustRule);

    virtual size_t external_memory_size() const override;
    virtual Utf16String serialized() const override;
    virtual void visit_edges(Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Utf16FlyString m_name;
    RustDeclarationBlock m_declarations;
    mutable GC::Ptr<CSSStyleProperties> m_style;
};

}
