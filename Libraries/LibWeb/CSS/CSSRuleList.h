/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TraversalOrder.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom/#the-cssrulelist-interface
class WEB_API CSSRuleList : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSRuleList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSRuleList);

public:
    [[nodiscard]] static GC::Ref<CSSRuleList> create(JS::Realm&, ReadonlySpan<GC::Ref<CSSRule>> = {});

    ~CSSRuleList() = default;

    CSSRule const* item(size_t index) const
    {
        if (index >= length())
            return nullptr;
        return m_rules[index];
    }

    CSSRule* item(size_t index)
    {
        if (index >= length())
            return nullptr;
        return m_rules[index];
    }

    size_t length() const { return m_rules.size(); }

    auto begin() const { return m_rules.begin(); }
    auto begin() { return m_rules.begin(); }

    auto end() const { return m_rules.end(); }
    auto end() { return m_rules.end(); }

    virtual Optional<JS::Value> item_value(size_t index) const override;

    WebIDL::ExceptionOr<void> remove_a_css_rule(u32 index);
    enum class Nested {
        No,
        Yes,
    };
    WebIDL::ExceptionOr<unsigned> insert_a_css_rule(Variant<StringView, CSSRule*>, u32 index, Nested, HashTable<FlyString> const& declared_namespaces);

    void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const;
    // Returns whether the match state of any media queries changed after evaluation.
    bool evaluate_media_queries(DOM::Document const&);

    void set_owner_rule(GC::Ref<CSSRule> owner_rule) { m_owner_rule = owner_rule; }
    void set_rules(Badge<CSSStyleSheet>, Vector<GC::Ref<CSSRule>> rules) { m_rules = move(rules); }

    Function<void()> on_change;

private:
    explicit CSSRuleList(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<Parser::RuleContext> rule_context() const;

    Vector<GC::Ref<CSSRule>> m_rules;
    GC::Ptr<CSSRule> m_owner_rule;
};

}
