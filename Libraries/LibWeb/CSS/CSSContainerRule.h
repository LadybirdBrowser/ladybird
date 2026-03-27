/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <LibWeb/CSS/CSSConditionRule.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-conditional-5/#dictdef-csscontainercondition
struct CSSContainerCondition {
    String name;
    String query;
};

// https://drafts.csswg.org/css-conditional-5/#the-csscontainerrule-interface
class CSSContainerRule final : public CSSConditionRule {
    WEB_PLATFORM_OBJECT(CSSContainerRule, CSSConditionRule);
    GC_DECLARE_ALLOCATOR(CSSContainerRule);

public:
    struct Condition {
        Optional<FlyString> container_name;
        RefPtr<ContainerQuery> container_query;
    };
    [[nodiscard]] static GC::Ref<CSSContainerRule> create(JS::Realm&, Vector<Condition>&&, CSSRuleList&);

    virtual ~CSSContainerRule() override;

    virtual String condition_text() const override;
    virtual bool condition_matches() const override;

    String container_name() const;
    String container_query() const;

    // FIXME: Should be FrozenArray
    Vector<CSSContainerCondition> conditions() const;

    virtual void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const override;

private:
    CSSContainerRule(JS::Realm&, Vector<Condition>&&, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;

    Vector<Condition> m_conditions;
};

template<>
inline bool CSSRule::fast_is<CSSContainerRule>() const { return type() == CSSRule::Type::Container; }

}
