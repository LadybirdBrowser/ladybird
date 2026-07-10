/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/CSS/CSSConditionRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-conditional-5/#dictdef-csscontainercondition
struct CSSContainerCondition {
    Utf16String name;
    Utf16String query;
};

// https://drafts.csswg.org/css-conditional-5/#the-csscontainerrule-interface
class CSSContainerRule final : public CSSConditionRule {
    WEB_PLATFORM_OBJECT(CSSContainerRule, CSSConditionRule);
    GC_DECLARE_ALLOCATOR(CSSContainerRule);

public:
    struct Condition {
        Optional<Utf16FlyString> container_name;
        RefPtr<ContainerQuery> container_query;
    };
    [[nodiscard]] static GC::Ref<CSSContainerRule> create(JS::Realm&, Vector<Condition>&&, CSSRuleList&);

    virtual ~CSSContainerRule() override;

    virtual Utf16String serialized_condition_text() const override;
    virtual bool condition_matches() const override;
    bool matches(DOM::AbstractElement const&) const;
    bool contains_size_feature() const;
    bool contains_style_feature() const;

    Utf16String container_name() const;
    Utf16String container_query() const;

    // FIXME: Should be FrozenArray
    Vector<CSSContainerCondition> conditions() const;

    virtual void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const override;

private:
    CSSContainerRule(JS::Realm&, Vector<Condition>&&, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void clear_caches() override;
    virtual Utf16String serialized() const override;
    CSSContainerRule const* find_parent_container_rule() const;

    bool conditions_match(DOM::AbstractElement const&) const;

    Vector<Condition> m_conditions;
    mutable GC::Ptr<CSSContainerRule const> m_cached_parent_container_rule;
    mutable bool m_parent_container_rule_cache_valid { false };
};

template<>
inline bool CSSRule::fast_is<CSSContainerRule>() const { return type() == CSSRule::Type::Container; }

}
