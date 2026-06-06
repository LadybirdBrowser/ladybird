/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/CSSConditionRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-conditional-3/#the-csssupportsrule-interface
class CSSSupportsRule final : public CSSConditionRule {
    WEB_WRAPPABLE(CSSSupportsRule, CSSConditionRule);
    GC_DECLARE_ALLOCATOR(CSSSupportsRule);

public:
    static GC::Ref<CSSSupportsRule> create(NonnullRefPtr<Supports>&&, CSSRuleList&);

    virtual ~CSSSupportsRule() = default;

    String condition_text() const override;
    bool matches() const { return condition_matches(); }

    virtual bool condition_matches() const override { return m_supports->matches(); }

    Supports const& supports() const { return m_supports; }

private:
    CSSSupportsRule(NonnullRefPtr<Supports>&&, CSSRuleList&);
    virtual String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    NonnullRefPtr<Supports> m_supports;
};

template<>
inline bool CSSRule::fast_is<CSSSupportsRule>() const { return type() == CSSRule::Type::Supports; }

}
