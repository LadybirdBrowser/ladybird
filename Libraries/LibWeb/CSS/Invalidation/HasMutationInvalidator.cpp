/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibWeb/CSS/Invalidation/HasMutationFeatureCollector.h>
#include <LibWeb/CSS/Invalidation/HasMutationInvalidator.h>
#include <LibWeb/CSS/Invalidation/InvalidationSetMatcher.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS::Invalidation {

static bool reason_may_affect_has_selectors(DOM::StyleInvalidationReason reason)
{
    // :has() selectors match based on DOM state only (structure, attributes, pseudo-classes). Reasons that don't change
    // any DOM state can't affect :has() matching, so we can skip scheduling :has() ancestor invalidation.
    switch (reason) {
    case DOM::StyleInvalidationReason::BaseURLChanged:
    case DOM::StyleInvalidationReason::CSSFontLoaded:
    case DOM::StyleInvalidationReason::HTMLIFrameElementGeometryChange:
    case DOM::StyleInvalidationReason::HTMLObjectElementUpdateLayoutAndChildObjects:
    case DOM::StyleInvalidationReason::NavigableSetViewportSize:
    case DOM::StyleInvalidationReason::SettingsChange:
        return false;
    default:
        return true;
    }
}

static void invalidate_children_affected_by_has_sibling_combinators(DOM::Node& parent)
{
    parent.for_each_child_of_type<DOM::Element>([&](auto& element) {
        if (element.affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator())
            invalidate_element_if_affected_by_has(element);
        return IterationDecision::Continue;
    });
}

static bool pending_has_invalidation_covers_all_child_list_mutation_features(StyleScope& scope, DOM::Node& parent)
{
    auto pending_invalidation = scope.m_pending_has_invalidations.find(parent);
    if (pending_invalidation == scope.m_pending_has_invalidations.end())
        return false;

    auto const& mutation_features = pending_invalidation->value;
    if (mutation_features.is_conservative)
        return true;

    auto const* data = scope.m_rule_cache ? &scope.m_rule_cache->style_invalidation_data : nullptr;
    if (!data)
        return false;

    if (!mutation_features.may_affect_sibling_relationships)
        return false;

    auto contains_all_keys = [](auto const& existing_features, auto const& used_features) {
        for (auto const& entry : used_features) {
            if (!existing_features.contains(entry.key))
                return false;
        }
        return true;
    };

    if (!contains_all_keys(mutation_features.tag_names, data->tag_names_used_in_has_selectors))
        return false;
    if (!contains_all_keys(mutation_features.ids, data->ids_used_in_has_selectors))
        return false;
    if (!contains_all_keys(mutation_features.class_names, data->class_names_used_in_has_selectors))
        return false;
    if (!contains_all_keys(mutation_features.attribute_names, data->attribute_names_used_in_has_selectors))
        return false;
    if (!data->pseudo_classes_used_in_has_selectors.is_empty() && !mutation_features.may_affect_pseudo_classes)
        return false;

    return true;
}

static bool scope_has_featureless_sensitive_has_selectors(StyleScope const& scope)
{
    auto const* data = scope.m_rule_cache ? &scope.m_rule_cache->style_invalidation_data : nullptr;
    return data && data->has_selectors_sensitive_to_featureless_subtree_changes;
}

void invalidate_element_if_affected_by_has(DOM::Element& element, DescendantHasInvalidation descendant_has_invalidation)
{
    if (element.affected_by_has_pseudo_class_in_subject_position())
        element.set_needs_style_update(true);
    if (descendant_has_invalidation == DescendantHasInvalidation::Yes && element.affected_by_has_pseudo_class_in_non_subject_position())
        element.invalidate_style(DOM::StyleInvalidationReason::Other, { { InvalidationSet::Property::Type::PseudoClass, PseudoClass::Has } }, {});
}

static bool is_in_has_scope(DOM::Element const& element)
{
    return element.in_has_scope()
        || element.affected_by_has_pseudo_class_in_subject_position()
        || element.affected_by_has_pseudo_class_in_non_subject_position();
}

static bool is_in_subtree_of_has_relative_selector_with_sibling_combinator(DOM::Element const& element)
{
    return element.in_subtree_of_has_pseudo_class_relative_selector_with_sibling_combinator()
        || element.affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator();
}

static bool attribute_may_match_mutation_features(Selector::SimpleSelector::Attribute const& attribute, PendingHasInvalidationMutationFeatures const& mutation_features)
{
    auto const& attribute_name = attribute.qualified_name.name.name;
    if (mutation_features.attribute_names.contains(attribute_name))
        return true;

    auto const& lowercase_attribute_name = attribute.qualified_name.name.lowercase_name;
    return lowercase_attribute_name != attribute_name && mutation_features.attribute_names.contains(lowercase_attribute_name);
}

static bool selector_may_match_mutation_features(Selector const& selector, PendingHasInvalidationMutationFeatures const& mutation_features)
{
    if (mutation_features.is_conservative)
        return true;
    Function<bool(Selector const&)> visit_selector = [&](Selector const& selector) {
        bool saw_concrete_feature = false;
        bool concrete_feature_found_in_mutation_subtree = false;
        bool must_be_conservative = false;

        auto visit_selector_list = [&](SelectorList const& selector_list) {
            for (auto const& argument_selector : selector_list) {
                if (visit_selector(*argument_selector))
                    return true;
            }
            return false;
        };

        for (auto const& compound_selector : selector.compound_selectors()) {
            if ((compound_selector.combinator == Selector::Combinator::NextSibling
                    || compound_selector.combinator == Selector::Combinator::SubsequentSibling)
                && mutation_features.may_affect_sibling_relationships)
                must_be_conservative = true;
            if (compound_selector.simple_selectors.is_empty()) {
                must_be_conservative = true;
                continue;
            }
            for (auto const& simple_selector : compound_selector.simple_selectors) {
                switch (simple_selector.type) {
                case Selector::SimpleSelector::Type::Universal:
                case Selector::SimpleSelector::Type::Nesting:
                case Selector::SimpleSelector::Type::Invalid:
                case Selector::SimpleSelector::Type::PseudoElement:
                    must_be_conservative = true;
                    break;
                case Selector::SimpleSelector::Type::TagName:
                    saw_concrete_feature = true;
                    concrete_feature_found_in_mutation_subtree |= mutation_features.tag_names.contains(simple_selector.qualified_name().name.lowercase_name);
                    break;
                case Selector::SimpleSelector::Type::Id:
                    saw_concrete_feature = true;
                    concrete_feature_found_in_mutation_subtree |= mutation_features.ids.contains(simple_selector.name());
                    break;
                case Selector::SimpleSelector::Type::Class:
                    saw_concrete_feature = true;
                    concrete_feature_found_in_mutation_subtree |= mutation_features.class_names.contains(simple_selector.name());
                    break;
                case Selector::SimpleSelector::Type::Attribute:
                    saw_concrete_feature = true;
                    concrete_feature_found_in_mutation_subtree |= attribute_may_match_mutation_features(simple_selector.attribute(), mutation_features);
                    break;
                case Selector::SimpleSelector::Type::PseudoClass: {
                    auto const& pseudo_class = simple_selector.pseudo_class();
                    switch (pseudo_class.type) {
                    case PseudoClass::Is:
                    case PseudoClass::Where:
                        saw_concrete_feature = true;
                        concrete_feature_found_in_mutation_subtree |= visit_selector_list(pseudo_class.argument_selector_list);
                        break;
                    case PseudoClass::Enabled:
                    case PseudoClass::Defined:
                    case PseudoClass::Disabled:
                    case PseudoClass::Empty:
                    case PseudoClass::PlaceholderShown:
                    case PseudoClass::Checked:
                    case PseudoClass::Dir:
                    case PseudoClass::Lang:
                    case PseudoClass::Link:
                    case PseudoClass::AnyLink:
                    case PseudoClass::LocalLink:
                    case PseudoClass::Required:
                    case PseudoClass::Optional:
                    case PseudoClass::Hover:
                    case PseudoClass::Focus:
                    case PseudoClass::FocusVisible:
                    case PseudoClass::FocusWithin:
                    case PseudoClass::Active:
                    case PseudoClass::Target:
                    case PseudoClass::Modal:
                    case PseudoClass::Open:
                    case PseudoClass::PopoverOpen:
                    case PseudoClass::Autofill:
                    case PseudoClass::Default:
                    case PseudoClass::Fullscreen:
                    case PseudoClass::Indeterminate:
                    case PseudoClass::Invalid:
                    case PseudoClass::Muted:
                    case PseudoClass::Paused:
                    case PseudoClass::Playing:
                    case PseudoClass::ReadOnly:
                    case PseudoClass::ReadWrite:
                    case PseudoClass::Seeking:
                    case PseudoClass::Stalled:
                    case PseudoClass::Unchecked:
                    case PseudoClass::UserInvalid:
                    case PseudoClass::UserValid:
                    case PseudoClass::Valid:
                    case PseudoClass::VolumeLocked:
                    case PseudoClass::Buffering:
                    case PseudoClass::HighValue:
                    case PseudoClass::LowValue:
                    case PseudoClass::OptimalValue:
                    case PseudoClass::SuboptimalValue:
                    case PseudoClass::EvenLessGoodValue:
                        saw_concrete_feature = true;
                        concrete_feature_found_in_mutation_subtree |= mutation_features.may_affect_pseudo_classes
                            || mutation_features.pseudo_classes.contains(pseudo_class.type);
                        break;
                    case PseudoClass::Not:
                    case PseudoClass::Has:
                    default:
                        must_be_conservative = true;
                        break;
                    }
                    break;
                }
                }
            }
        }

        if (must_be_conservative)
            return true;
        return !saw_concrete_feature || concrete_feature_found_in_mutation_subtree;
    };

    return visit_selector(selector);
}

static bool has_rule_that_may_be_affected_by_mutation(StyleScope& style_scope, DOM::Element const& anchor, PendingHasInvalidationMutationFeatures const& mutation_features)
{
    bool found_has_rule = false;
    bool may_be_affected = false;

    auto check_selector = [&](Selector const& selector) {
        Function<void(Selector const&)> visit_selector = [&](Selector const& selector) {
            if (may_be_affected)
                return;
            for (auto const& compound_selector : selector.compound_selectors()) {
                for (auto const& simple_selector : compound_selector.simple_selectors) {
                    if (simple_selector.type != Selector::SimpleSelector::Type::PseudoClass)
                        continue;
                    auto const& pseudo_class = simple_selector.pseudo_class();
                    if (pseudo_class.type == PseudoClass::Has) {
                        if (!compound_may_match_element(anchor, compound_selector, PseudoClass::Has))
                            continue;
                        found_has_rule = true;
                        for (auto const& argument_selector : pseudo_class.argument_selector_list) {
                            if (selector_may_match_mutation_features(*argument_selector, mutation_features)) {
                                may_be_affected = true;
                                return;
                            }
                        }
                    }
                    for (auto const& argument_selector : pseudo_class.argument_selector_list)
                        visit_selector(*argument_selector);
                }
            }
        };
        visit_selector(selector);
    };

    auto check_rule_vector = [&](Vector<MatchingRule> const& rules) {
        for (auto const& rule : rules) {
            check_selector(rule.selector);
            if (may_be_affected)
                return;
        }
    };

    auto const& has_rule_cache = style_scope.get_pseudo_class_rule_cache(PseudoClass::Has);
    for (auto const& entry : has_rule_cache.rules_by_id)
        check_rule_vector(entry.value);
    for (auto const& entry : has_rule_cache.rules_by_class)
        check_rule_vector(entry.value);
    for (auto const& entry : has_rule_cache.rules_by_tag_name)
        check_rule_vector(entry.value);
    for (auto const& entry : has_rule_cache.rules_by_attribute_name)
        check_rule_vector(entry.value);
    for (auto const& rules : has_rule_cache.rules_by_pseudo_element)
        check_rule_vector(rules);
    check_rule_vector(has_rule_cache.root_rules);
    check_rule_vector(has_rule_cache.slotted_rules);
    check_rule_vector(has_rule_cache.part_rules);
    check_rule_vector(has_rule_cache.other_rules);

    return !found_has_rule || may_be_affected;
}

static void invalidate_style_of_elements_affected_by_pending_has_mutations(StyleScope& style_scope)
{
    if (style_scope.m_pending_has_invalidations.is_empty())
        return;

    ScopeGuard clear_pending_nodes_guard = [&] {
        style_scope.m_pending_has_invalidations.clear();
    };

    auto& counters = style_scope.document().style_invalidation_counters();
    if (!style_scope.has_valid_rule_cache())
        ++counters.has_invalidation_rule_cache_builds;

    // It's ok to call have_has_selectors() instead of may_have_has_selectors() here and force
    // rule cache build, because it's going to be built soon anyway, since we could get here
    // only from update_style().
    if (!style_scope.have_has_selectors())
        return;

    ++counters.has_ancestor_walk_invocations;

    GC::RootHashMap<GC::Ref<DOM::Element>, DescendantHasInvalidation> invalidated_elements;
    auto invalidate_element = [&](GC::Ref<DOM::Element> element, DescendantHasInvalidation descendant_has_invalidation) {
        auto previous_invalidation = invalidated_elements.find(element);
        if (previous_invalidation != invalidated_elements.end()) {
            if (previous_invalidation->value == DescendantHasInvalidation::Yes
                || descendant_has_invalidation == DescendantHasInvalidation::No) {
                return false;
            }
        }
        invalidated_elements.set(element, descendant_has_invalidation);
        invalidate_element_if_affected_by_has(*element, descendant_has_invalidation);
        return true;
    };

    GC::OrderedRootHashMap<GC::Ref<DOM::Node>, PendingHasInvalidationMutationFeatures> pending_has_invalidations;
    for (auto& [node, features] : style_scope.m_pending_has_invalidations)
        pending_has_invalidations.set(node, features);
    bool should_scan_ancestor_siblings = style_scope.have_has_selectors_with_relative_selector_that_has_sibling_combinator();
    for (auto& [node, mutation_features] : pending_has_invalidations) {
        GC::RootHashTable<GC::Ref<DOM::Element>> elements_skipped_by_has_feature_filter;
        GC::RootVector<GC::Ref<DOM::Element>, 16> has_scope_ancestors;
        bool should_delay_ancestor_sibling_scans = false;
        for (GC::Ptr<DOM::Node> ancestor = node; ancestor; ancestor = ancestor->parent_or_shadow_host()) {
            if (!ancestor->is_element())
                continue;
            GC::Ref<DOM::Element> element = static_cast<DOM::Element&>(*ancestor);

            // Terminate the upward walk once we reach an element that no :has()
            // anchor has ever observed. Its style cannot be affected by a mutation
            // further down the tree, so neither can anything above it.
            if (!is_in_has_scope(*element))
                break;

            has_scope_ancestors.append(element);
            should_delay_ancestor_sibling_scans |= is_in_subtree_of_has_relative_selector_with_sibling_combinator(*element);
        }

        for (auto element : has_scope_ancestors) {
            auto previous_invalidation = invalidated_elements.find(element);
            if (previous_invalidation != invalidated_elements.end() && previous_invalidation->value == DescendantHasInvalidation::Yes)
                continue;

            ++counters.has_ancestor_walk_visits;
            bool can_skip_unchanged_has_fanout = !element->root().is_shadow_root() && !element->assigned_slot_internal() && !element->is_shadow_host();
            bool should_invalidate_descendants = element->affected_by_has_pseudo_class_in_non_subject_position();
            if (should_invalidate_descendants && can_skip_unchanged_has_fanout)
                should_invalidate_descendants = has_rule_that_may_be_affected_by_mutation(style_scope, element, mutation_features);
            if (element->affected_by_has_pseudo_class_in_subject_position() || should_invalidate_descendants) {
                invalidate_element(element, should_invalidate_descendants ? DescendantHasInvalidation::Yes : DescendantHasInvalidation::No);
            } else {
                elements_skipped_by_has_feature_filter.set(element);
            }

            GC::Ptr<DOM::Node> parent = element->parent_or_shadow_host();
            if (!parent)
                return;

            // If any ancestor's sibling was tested against selectors like ".a:has(+ .b)" or ".a:has(~ .b)"
            // its style might be affected by the change in descendant node.
            if (!should_scan_ancestor_siblings)
                continue;
            if (should_delay_ancestor_sibling_scans && !is_in_subtree_of_has_relative_selector_with_sibling_combinator(element))
                continue;
            parent->for_each_child_of_type<DOM::Element>([&](auto& ancestor_sibling_element) {
                ++counters.has_ancestor_sibling_element_checks;
                if (ancestor_sibling_element.affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator()) {
                    GC::Ref<DOM::Element> ancestor_sibling = ancestor_sibling_element;
                    if (elements_skipped_by_has_feature_filter.contains(ancestor_sibling))
                        return IterationDecision::Continue;
                    if (!invalidate_element(ancestor_sibling, DescendantHasInvalidation::Yes))
                        return IterationDecision::Continue;

                    ++counters.has_ancestor_walk_visits;
                }
                return IterationDecision::Continue;
            });
        }
    }
}

void invalidate_style_for_pending_has_mutations(DOM::Document& document)
{
    invalidate_style_of_elements_affected_by_pending_has_mutations(document.style_scope());
    document.for_each_shadow_root([&](auto& shadow_root) {
        bool has_active_style_sheets = false;
        shadow_root.for_each_active_css_style_sheet([&](auto&) {
            has_active_style_sheets = true;
        });
        if (!has_active_style_sheets) {
            // Without shadow stylesheets, this scope cannot contain :has() selectors.
            // Document-level user rules are handled by the document style scope above.
            shadow_root.style_scope().m_pending_has_invalidations.clear();
            return;
        }
        invalidate_style_of_elements_affected_by_pending_has_mutations(shadow_root.style_scope());
    });
}

static void schedule_has_invalidation_for_child_list_mutation(DOM::Node& parent, DOM::Node& mutation_root, StyleScope& scope)
{
    if (!scope.may_have_has_selectors())
        return;

    auto has_sibling_combinator_has_selectors = scope.may_have_has_selectors_with_relative_selector_that_has_sibling_combinator();

    if (pending_has_invalidation_covers_all_child_list_mutation_features(scope, parent))
        return;

    if (scope_has_featureless_sensitive_has_selectors(scope)) {
        scope.record_conservative_pending_has_invalidation(parent, true);
        if (has_sibling_combinator_has_selectors)
            invalidate_children_affected_by_has_sibling_combinators(parent);
        return;
    }

    // Sibling-combinator :has() selectors are sensitive to featureless insertions/removals because a plain node can
    // still change adjacency and following-sibling relationships.
    auto may_affect_has_match = mutation_root.is_character_data()
        || subtree_has_feature_used_in_has_selector(mutation_root, scope)
        || has_sibling_combinator_has_selectors;
    if (!may_affect_has_match)
        return;

    scope.record_pending_has_invalidation_mutation_features(parent, mutation_root, true);
    scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(parent);

    if (has_sibling_combinator_has_selectors)
        invalidate_children_affected_by_has_sibling_combinators(parent);
}

static void schedule_has_invalidation_for_node_in_scope(DOM::Node& node, StyleScope& style_scope)
{
    if (!style_scope.may_have_has_selectors())
        return;

    style_scope.record_pending_has_invalidation_mutation_features(node, node, false);
    style_scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(node);
}

static void schedule_document_user_has_invalidation_for_shadow_node(DOM::Node& node, StyleScope& node_style_scope)
{
    if (!is<DOM::ShadowRoot>(node.root()))
        return;

    auto& document_style_scope = node.document().style_scope();
    if (&node_style_scope == &document_style_scope)
        return;

    if (!document_style_scope.may_have_user_has_selectors())
        return;

    document_style_scope.record_pending_has_invalidation_mutation_features(node, node, false);
    document_style_scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(node);
}

void schedule_has_invalidation_for_node(DOM::Node& node, DOM::StyleInvalidationReason reason)
{
    auto is_child_list_mutation = reason == DOM::StyleInvalidationReason::NodeRemove
        || reason == DOM::StyleInvalidationReason::NodeInsertBefore;

    // On insertion and removal the mutated node itself is uninteresting to the
    // :has() walker (a freshly inserted node has no :has() scope flags yet, and
    // a removed node is about to leave the tree). Start the walk at the parent,
    // which was in scope before and reliably carries the correct flags.
    if (is_child_list_mutation) {
        auto* parent = node.parent_or_shadow_host();
        if (!parent)
            return;

        // Walk every scope that can observe the parent, including enclosing and hosted shadow roots, so :has() in
        // :host(), ::slotted(), and ::part() selectors can react to the mutation.
        parent->for_each_style_scope_which_may_observe_the_node([&](StyleScope& scope) {
            schedule_has_invalidation_for_child_list_mutation(*parent, node, scope);
        });
        return;
    }

    if (!reason_may_affect_has_selectors(reason))
        return;

    auto& style_scope = node.style_scope();
    schedule_has_invalidation_for_node_in_scope(node, style_scope);
    schedule_document_user_has_invalidation_for_shadow_node(node, style_scope);
}

void schedule_has_invalidation_for_same_parent_move(DOM::Node& node)
{
    auto* parent = node.parent_or_shadow_host();
    if (!parent)
        return;

    parent->for_each_style_scope_which_may_observe_the_node([&](StyleScope& scope) {
        schedule_has_invalidation_for_child_list_mutation(*parent, node, scope);
    });
}

}
