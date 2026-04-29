/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleInvalidationData.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class StyleScope;

struct MatchingRule {
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
    bool has_has_selectors_with_relative_selector_that_has_sibling_combinator { false };
};

struct StyleCache : public RefCounted<StyleCache> {
    static NonnullRefPtr<StyleCache> create();
    static NonnullRefPtr<StyleCache> create_for_style_scope(StyleScope&);

    Vector<FlyString> qualified_layer_names_in_order;
    SelectorInsights selector_insights;
    Array<OwnPtr<RuleCache>, to_underlying(PseudoClass::__Count)> pseudo_class_rule_cache;
    StyleInvalidationData style_invalidation_data;
    RuleCaches author_rule_cache;
    RuleCaches user_rule_cache;
    RuleCaches user_agent_rule_cache;

    void visit_edges(GC::Cell::Visitor&);
};

struct PendingHasInvalidationMutationFeatures {
    bool is_conservative { false };
    bool may_affect_sibling_relationships { false };
    bool may_affect_pseudo_classes { false };
    HashTable<FlyString> tag_names;
    HashTable<FlyString> ids;
    HashTable<FlyString> class_names;
    HashTable<FlyString> attribute_names;
    HashTable<PseudoClass> pseudo_classes;
};

class StyleScope {
public:
    explicit StyleScope(GC::Ref<DOM::Node>);

    DOM::Node& node() const { return m_node; }
    DOM::Document& document() const;

    RuleCaches const& author_rule_cache() const { return m_rule_cache->author_rule_cache; }
    RuleCaches const& user_rule_cache() const { return m_rule_cache->user_rule_cache; }
    RuleCaches const& user_agent_rule_cache() const { return m_rule_cache->user_agent_rule_cache; }

    [[nodiscard]] bool has_valid_rule_cache() const { return m_rule_cache; }
    void invalidate_rule_cache();

    [[nodiscard]] RuleCache const& get_pseudo_class_rule_cache(PseudoClass) const;

    void for_each_stylesheet(CascadeOrigin, Function<void(CSS::CSSStyleSheet&)> const&) const;
    void build_user_style_sheet_if_needed();

    void make_rule_cache_for_cascade_origin(CascadeOrigin, StyleCache&);

    void build_rule_cache();
    void build_rule_cache_if_needed() const;
    void populate_rule_cache(StyleCache&);

    static void collect_selector_insights(Selector const&, SelectorInsights&);

    void build_qualified_layer_names_cache(StyleCache&);

    [[nodiscard]] bool may_have_has_selectors() const;
    [[nodiscard]] bool have_has_selectors() const;
    [[nodiscard]] bool may_have_has_selectors_with_relative_selector_that_has_sibling_combinator() const;
    [[nodiscard]] bool have_has_selectors_with_relative_selector_that_has_sibling_combinator() const;

    void for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const;

    void invalidate_counter_style_cache();
    void build_counter_style_cache();
    RefPtr<CSS::CounterStyle const> get_registered_counter_style(FlyString const& name) const;

    void schedule_ancestors_style_invalidation_due_to_presence_of_has(GC::Ref<DOM::Node>);
    void record_pending_has_invalidation_mutation_features(GC::Ref<DOM::Node>, GC::Ref<DOM::Node>, bool includes_descendants);
    void record_pending_has_invalidation_mutation_features(GC::Ref<DOM::Node>, Vector<CSS::InvalidationSet::Property> const&);

    template<typename T>
    Optional<T> dereference_global_tree_scoped_reference(Function<Optional<T>(StyleScope const&)> const& callback) const;

    void visit_edges(GC::Cell::Visitor&);

    RefPtr<StyleCache> m_rule_cache;

    GC::Ptr<CSSStyleSheet> m_user_style_sheet;

    OrderedHashMap<GC::Ref<DOM::Node>, PendingHasInvalidationMutationFeatures> m_pending_has_invalidations;

    bool m_needs_counter_style_cache_update : 1 { true };
    bool m_is_doing_counter_style_cache_update : 1 { false };
    HashMap<FlyString, NonnullRefPtr<CSS::CounterStyle const>> m_registered_counter_styles;

    GC::Ref<DOM::Node> m_node;
};

}
