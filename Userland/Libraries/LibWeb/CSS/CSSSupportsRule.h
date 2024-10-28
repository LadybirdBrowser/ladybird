/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
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
    WEB_PLATFORM_OBJECT(CSSSupportsRule, CSSConditionRule);
    JS_DECLARE_ALLOCATOR(CSSSupportsRule);

public:
    static JS::NonnullGCPtr<CSSSupportsRule> create(JS::Realm&, NonnullRefPtr<Supports>&&, CSSRuleList&);

    virtual ~CSSSupportsRule() = default;

    String condition_text() const override;
    virtual bool condition_matches() const override { return m_supports->matches(); }

private:
    CSSSupportsRule(JS::Realm&, NonnullRefPtr<Supports>&&, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;

    NonnullRefPtr<Supports> m_supports;
};

template<>
inline bool CSSRule::fast_is<CSSSupportsRule>() const { return type() == CSSRule::Type::Supports; }

}
