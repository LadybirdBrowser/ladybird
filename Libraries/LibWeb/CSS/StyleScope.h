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
#include <LibWeb/CSS/SelectorInsights.h>
#include <LibWeb/CSS/StyleInvalidationData.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class StyleScope;

struct MatchingRule {
    GC::Ptr<CSSRule const> rule; // Either CSSStyleRule or CSSNestedDeclarations
    GC::Ptr<CSSStyleSheet const> sheet;
    GC::Ptr<CSSContainerRule const> container_rule;
    GC::Ptr<CSSRule const> scope_rule; // Either CSSScopeRule or CSSImportRule
    Optional<FlyString> default_namespace;
    Selector const& selector;
    size_t selector_index { 0 };
    size_t style_sheet_index { 0 };
    size_t rule_index { 0 };

    u32 specificity { 0 };
    u32 multi_bucket_rule_index { 0 };
    CascadeOrigin cascade_origin;
    bool contains_pseudo_element { false };
    bool slotted { false };
    bool contains_part_pseudo_element { false };

    // Helpers to deal with the fact that `rule` might be a CSSStyleRule or a CSSNestedDeclarations
    CSSStyleProperties const& declaration() const;
    SelectorList const& absolutized_selectors() const;
    FlyString const& qualified_layer_name() const;

    void visit_edges(GC::Cell::Visitor&) const;
};

enum class SubjectPseudoClassBuckets {
    No,
    Yes,
};

enum class AncestorHashBuckets {
    No,
    Yes,
};

struct RuleCache {
    HashMap<FlyString, Vector<MatchingRule>> rules_by_id;
    HashMap<FlyString, Vector<MatchingRule>> rules_by_class;
    HashMap<FlyString, Vector<MatchingRule>> rules_by_tag_name;
    HashMap<FlyString, Vector<MatchingRule>, AK::ASCIICaseInsensitiveFlyStringTraits> rules_by_attribute_name;
    Array<Vector<MatchingRule>, to_underlying(PseudoClass::__Count)> rules_by_subject_pseudo_class;
    HashMap<u32, Vector<MatchingRule>> rules_by_ancestor_hash;
    Vector<MatchingRule> root_rules;
    Vector<MatchingRule> slotted_rules;
    Vector<MatchingRule> part_rules;
    Vector<MatchingRule> other_rules;

    struct PseudoElementRules {
        HashMap<FlyString, Vector<MatchingRule>> rules_by_id;
        HashMap<FlyString, Vector<MatchingRule>> rules_by_class;
        HashMap<FlyString, Vector<MatchingRule>> rules_by_tag_name;
        HashMap<FlyString, Vector<MatchingRule>, AK::ASCIICaseInsensitiveFlyStringTraits> rules_by_attribute_name;
        Array<Vector<MatchingRule>, to_underlying(PseudoClass::__Count)> rules_by_subject_pseudo_class;
        HashMap<u32, Vector<MatchingRule>> rules_by_ancestor_hash;
        Vector<MatchingRule> root_rules;
        Vector<MatchingRule> other_rules;
    };
    Array<PseudoElementRules, to_underlying(CSS::PseudoElement::KnownPseudoElementCount)> rules_by_pseudo_element;

    HashMap<FlyString, NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet>> rules_by_animation_keyframes;

    u32 next_multi_bucket_rule_index { 0 };

    void add_rule(MatchingRule const&, Optional<PseudoElement>, bool contains_root_pseudo_class, SubjectPseudoClassBuckets, AncestorHashBuckets);
    void for_each_matching_rules(DOM::AbstractElement, Function<bool(u32)> const& may_contain_ancestor_hash, Function<IterationDecision(Vector<MatchingRule> const&)> callback) const;
    void for_each_matching_pseudo_element_rules(DOM::AbstractElement, Function<bool(u32)> const& may_contain_ancestor_hash, Function<IterationDecision(Vector<MatchingRule> const&)> callback) const;

    void visit_edges(GC::Cell::Visitor&);
};

struct RuleCaches {
    RuleCache main;
    HashMap<FlyString, NonnullOwnPtr<RuleCache>> by_layer;

    void visit_edges(GC::Cell::Visitor&);
};

struct StyleRuleCache {
    StyleRuleCache();

    Vector<FlyString> qualified_layer_names_in_order;
    SelectorInsights selector_insights;
    Array<OwnPtr<RuleCache>, to_underlying(PseudoClass::__Count)> pseudo_class_rule_cache;
    RuleCaches author_rule_cache;
    RuleCaches user_rule_cache;
    RuleCaches user_agent_rule_cache;
    bool has_size_container_queries { false };

    void visit_edges(GC::Cell::Visitor&);
};

struct StyleCache : public RefCounted<StyleCache> {
    static NonnullRefPtr<StyleCache> create();

    OwnPtr<StyleRuleCache> rule_cache;
    OwnPtr<StyleInvalidationData> style_invalidation_data;

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

    RuleCaches const& author_rule_cache() const { return rule_cache().author_rule_cache; }
    RuleCaches const& user_rule_cache() const { return rule_cache().user_rule_cache; }
    RuleCaches const& user_agent_rule_cache() const { return rule_cache().user_agent_rule_cache; }

    [[nodiscard]] StyleRuleCache const& rule_cache() const;
    [[nodiscard]] StyleInvalidationData const& style_invalidation_data() const;
    [[nodiscard]] bool has_valid_rule_cache() const { return m_style_cache && m_style_cache->rule_cache; }
    [[nodiscard]] bool has_valid_style_invalidation_data() const { return m_style_cache && m_style_cache->style_invalidation_data; }
    void invalidate_style_cache();
    void invalidate_user_style_sheet();

    [[nodiscard]] RuleCache const& get_pseudo_class_rule_cache(PseudoClass) const;

    void for_each_stylesheet(CascadeOrigin, Function<void(CSS::CSSStyleSheet&)> const&) const;
    static WEB_API void for_each_user_agent_stylesheet(bool include_quirks_mode_stylesheet, Function<void(CSS::CSSStyleSheet&, StyleSheetIdentifier const&)> const&);
    static Optional<StyleSheetIdentifier> user_agent_style_sheet_identifier(CSS::CSSStyleSheet const&);
    void build_user_style_sheet_if_needed();

    void make_rule_cache_for_cascade_origin(CascadeOrigin, StyleRuleCache&);
    void build_style_invalidation_data_for_cascade_origin(CascadeOrigin, StyleInvalidationData&);

    void build_rule_cache();
    void build_rule_cache_if_needed() const;
    void build_style_invalidation_data();
    void build_style_invalidation_data_if_needed() const;
    void populate_rule_cache(StyleRuleCache&);
    void populate_style_invalidation_data(StyleInvalidationData&);

    static void collect_selector_insights(Selector const&, SelectorInsights&);

    void build_qualified_layer_names_cache(StyleRuleCache&);

    [[nodiscard]] bool may_have_has_selectors() const;
    [[nodiscard]] bool may_have_user_has_selectors() const;
    [[nodiscard]] bool may_have_user_pseudo_class_selectors(PseudoClass) const;
    [[nodiscard]] bool have_has_selectors() const;
    [[nodiscard]] bool may_have_has_selectors_with_relative_selector_that_has_sibling_combinator() const;
    [[nodiscard]] bool have_has_selectors_with_relative_selector_that_has_sibling_combinator() const;
    [[nodiscard]] bool have_size_container_queries() const;

    void for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const;

    void invalidate_counter_style_cache();
    void build_counter_style_cache();
    RefPtr<CSS::CounterStyle const> get_registered_counter_style(FlyString const& name) const;

    void schedule_ancestors_style_invalidation_due_to_presence_of_has(GC::Ref<DOM::Node>);
    void record_conservative_pending_has_invalidation(GC::Ref<DOM::Node>, bool may_affect_sibling_relationships);
    void record_pending_has_invalidation_mutation_features(GC::Ref<DOM::Node>, GC::Ref<DOM::Node>, bool includes_descendants);
    void record_pending_has_invalidation_mutation_features(GC::Ref<DOM::Node>, Vector<CSS::InvalidationSet::Property> const&);

    template<typename T>
    Optional<T> dereference_global_tree_scoped_reference(Function<Optional<T>(StyleScope const&)> const& callback) const;

    void visit_edges(GC::Cell::Visitor&);

    StyleCache& ensure_style_cache();
    StyleCache& ensure_style_cache() const;

    RefPtr<StyleCache> m_style_cache;

    GC::Ptr<CSSStyleSheet> m_user_style_sheet;

    OrderedHashMap<GC::Ref<DOM::Node>, PendingHasInvalidationMutationFeatures> m_pending_has_invalidations;

    bool m_needs_counter_style_cache_update : 1 { true };
    bool m_is_doing_counter_style_cache_update : 1 { false };
    HashMap<FlyString, NonnullRefPtr<CSS::CounterStyle const>> m_registered_counter_styles;

    GC::Ref<DOM::Node> m_node;
};

}
