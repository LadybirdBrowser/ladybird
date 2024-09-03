/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Iterator.h>
#include <AK/RefPtr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TraversalOrder.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom/#the-cssrulelist-interface
class CSSRuleList : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSRuleList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(CSSRuleList);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSRuleList> create(JS::Realm&, JS::MarkedVector<CSSRule*> const&);
    [[nodiscard]] static JS::NonnullGCPtr<CSSRuleList> create_empty(JS::Realm&);

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
    WebIDL::ExceptionOr<unsigned> insert_a_css_rule(Variant<StringView, CSSRule*>, u32 index);

    void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const;
    // Returns whether the match state of any media queries changed after evaluation.
    bool evaluate_media_queries(HTML::Window const&);

    void set_rules(Badge<CSSStyleSheet>, Vector<JS::NonnullGCPtr<CSSRule>> rules) { m_rules = move(rules); }

    Function<void()> on_change;

private:
    explicit CSSRuleList(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<JS::NonnullGCPtr<CSSRule>> m_rules;
};

}
