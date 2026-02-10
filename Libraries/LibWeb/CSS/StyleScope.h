/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleInvalidationData.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct MatchingRule {
    GC::Ptr<DOM::ShadowRoot const> shadow_root;
    GC::Ptr<CSSRule const> rule; // Either CSSStyleRule or CSSNestedDeclarations
    GC::Ptr<CSSStyleSheet const> sheet;
    Optional<FlyString> default_namespace;
    Selector const& selector;
    size_t style_sheet_index { 0 };
    size_t rule_index { 0 };

    u32 specificity { 0 };
    CascadeOrigin cascade_origin;
    bool contains_pseudo_element { false };
    bool slotted { false };
    bool contains_part_pseudo_element { false };

    // Helpers to deal with the fact that `rule` might be a CSSStyleRule or a CSSNestedDeclarations
    CSSStyleProperties const& declaration() const;
    SelectorList const& absolutized_selectors() const;
    FlyString const& qualified_layer_name() const;

    void visit_edges(GC::Cell::Visitor&);
};

struct RuleCache {
    HashMap<FlyString, Vector<MatchingRule>> rules_by_id;
    HashMap<FlyString, Vector<MatchingRule>> rules_by_class;
    HashMap<FlyString, Vector<MatchingRule>> rules_by_tag_name;
    HashMap<FlyString, Vector<MatchingRule>, AK::ASCIICaseInsensitiveFlyStringTraits> rules_by_attribute_name;
    Array<Vector<MatchingRule>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount)> rules_by_pseudo_element;
    Vector<MatchingRule> root_rules;
    Vector<MatchingRule> slotted_rules;
    Vector<MatchingRule> part_rules;
    Vector<MatchingRule> other_rules;

    HashMap<FlyString, NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet>> rules_by_animation_keyframes;

    void add_rule(MatchingRule const&, Optional<PseudoElement>, bool contains_root_pseudo_class);
    void for_each_matching_rules(DOM::AbstractElement, Function<IterationDecision(Vector<MatchingRule> const&)> callback) const;

    void visit_edges(GC::Cell::Visitor&);
};

struct RuleCaches {
    RuleCache main;
    HashMap<FlyString, NonnullOwnPtr<RuleCache>> by_layer;

    void visit_edges(GC::Cell::Visitor&);
};

struct SelectorInsights {
    bool has_has_selectors { false };
};

class StyleScope {
public:
    explicit StyleScope(GC::Ref<DOM::Node>);

    DOM::Node& node() const { return m_node; }
    DOM::Document& document() const;

    RuleCaches const& author_rule_cache() const { return *m_author_rule_cache; }
    RuleCaches const& user_rule_cache() const { return *m_user_rule_cache; }
    RuleCaches const& user_agent_rule_cache() const { return *m_user_agent_rule_cache; }

    [[nodiscard]] bool has_valid_rule_cache() const { return m_author_rule_cache; }
    void invalidate_rule_cache();

    [[nodiscard]] RuleCache const& get_pseudo_class_rule_cache(PseudoClass) const;

    template<typename Callback>
    void for_each_stylesheet(CascadeOrigin, Callback) const;

    void make_rule_cache_for_cascade_origin(CascadeOrigin, SelectorInsights&);

    void build_rule_cache();
    void build_rule_cache_if_needed() const;

    static void collect_selector_insights(Selector const&, SelectorInsights&);

    void build_qualified_layer_names_cache();

    [[nodiscard]] bool may_have_has_selectors() const;
    [[nodiscard]] bool have_has_selectors() const;

    void for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)>&& callback) const;

    void invalidate_style_of_elements_affected_by_has();

    void schedule_ancestors_style_invalidation_due_to_presence_of_has(DOM::Node& node);

    void visit_edges(GC::Cell::Visitor&);

    Vector<FlyString> m_qualified_layer_names_in_order;
    OwnPtr<SelectorInsights> m_selector_insights;
    Array<OwnPtr<RuleCache>, to_underlying(PseudoClass::__Count)> m_pseudo_class_rule_cache;
    OwnPtr<StyleInvalidationData> m_style_invalidation_data;
    OwnPtr<RuleCaches> m_author_rule_cache;
    OwnPtr<RuleCaches> m_user_rule_cache;
    OwnPtr<RuleCaches> m_user_agent_rule_cache;

    GC::Ptr<CSSStyleSheet> m_user_style_sheet;

    HashTable<GC::Weak<DOM::Node>> m_pending_nodes_for_style_invalidation_due_to_presence_of_has;

    GC::Ref<DOM::Node> m_node;
};

}
