/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/HasMutationFeatureCollector.h>
#include <LibWeb/CSS/Invalidation/HasMutationInvalidator.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>

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

void invalidate_element_if_affected_by_has(DOM::Element& element)
{
    if (element.affected_by_has_pseudo_class_in_subject_position())
        element.set_needs_style_update(true);
    if (element.affected_by_has_pseudo_class_in_non_subject_position())
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
                    concrete_feature_found_in_mutation_subtree |= mutation_features.attribute_names.contains(simple_selector.attribute().qualified_name.name.lowercase_name);
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
                        saw_concrete_feature = true;
                        concrete_feature_found_in_mutation_subtree |= mutation_features.may_affect_pseudo_classes
                            || mutation_features.pseudo_classes.contains(pseudo_class.type);
                        break;
                    case PseudoClass::Hover:
                    case PseudoClass::Focus:
                    case PseudoClass::FocusVisible:
                    case PseudoClass::FocusWithin:
                    case PseudoClass::Active:
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

static bool compound_may_match_anchor_ignoring_has(Selector::CompoundSelector const& compound_selector, DOM::Element const& anchor)
{
    for (auto const& simple_selector : compound_selector.simple_selectors) {
        switch (simple_selector.type) {
        case Selector::SimpleSelector::Type::Universal:
            break;
        case Selector::SimpleSelector::Type::Nesting:
        case Selector::SimpleSelector::Type::Invalid:
        case Selector::SimpleSelector::Type::PseudoElement:
            return true;
        case Selector::SimpleSelector::Type::TagName:
            if (anchor.local_name() != simple_selector.qualified_name().name.lowercase_name)
                return false;
            break;
        case Selector::SimpleSelector::Type::Id: {
            auto id = anchor.id();
            if (!id.has_value() || *id != simple_selector.name())
                return false;
            break;
        }
        case Selector::SimpleSelector::Type::Class:
            if (!anchor.class_names().contains_slow(simple_selector.name()))
                return false;
            break;
        case Selector::SimpleSelector::Type::Attribute: {
            bool has_attribute = false;
            anchor.for_each_attribute([&](FlyString const& name, String const&) {
                if (name == simple_selector.attribute().qualified_name.name.lowercase_name)
                    has_attribute = true;
            });
            if (!has_attribute)
                return false;
            break;
        }
        case Selector::SimpleSelector::Type::PseudoClass:
            switch (simple_selector.pseudo_class().type) {
            case PseudoClass::Has:
                break;
            case PseudoClass::Root:
                if (&anchor != anchor.document().document_element())
                    return false;
                break;
            default:
                return true;
            }
            break;
        }
    }
    return true;
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
                        if (!compound_may_match_anchor_ignoring_has(compound_selector, anchor))
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

    // It's ok to call have_has_selectors() instead of may_have_has_selectors() here and force
    // rule cache build, because it's going to be built soon anyway, since we could get here
    // only from update_style().
    if (!style_scope.have_has_selectors())
        return;

    auto& counters = style_scope.document().style_invalidation_counters();
    ++counters.has_ancestor_walk_invocations;

    HashTable<GC::Ref<DOM::Element>> elements_already_invalidated_for_has;
    auto pending_has_invalidations = style_scope.m_pending_has_invalidations;
    bool should_scan_ancestor_siblings = style_scope.have_has_selectors_with_relative_selector_that_has_sibling_combinator();
    for (auto& [node, mutation_features] : pending_has_invalidations) {
        HashTable<GC::Ref<DOM::Element>> elements_skipped_by_has_feature_filter;
        Vector<GC::Ref<DOM::Element>, 16> has_scope_ancestors;
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
            if (elements_already_invalidated_for_has.contains(element))
                continue;

            ++counters.has_ancestor_walk_visits;
            bool can_skip_unchanged_has_fanout = !element->root().is_shadow_root() && !element->assigned_slot_internal() && !element->is_shadow_host();
            bool should_invalidate_element = true;
            if (element->affected_by_has_pseudo_class_in_non_subject_position() && can_skip_unchanged_has_fanout)
                should_invalidate_element = has_rule_that_may_be_affected_by_mutation(style_scope, element, mutation_features);
            if (should_invalidate_element) {
                elements_already_invalidated_for_has.set(element);
                invalidate_element_if_affected_by_has(*element);
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
                    if (elements_already_invalidated_for_has.set(ancestor_sibling) != AK::HashSetResult::InsertedNewEntry)
                        return IterationDecision::Continue;

                    ++counters.has_ancestor_walk_visits;
                    invalidate_element_if_affected_by_has(*ancestor_sibling);
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
        invalidate_style_of_elements_affected_by_pending_has_mutations(shadow_root.style_scope());
    });
}

static void schedule_has_invalidation_for_child_list_mutation(DOM::Node& parent, DOM::Node& mutation_root, StyleScope& scope)
{
    if (!scope.may_have_has_selectors())
        return;

    auto has_sibling_combinator_has_selectors = scope.may_have_has_selectors_with_relative_selector_that_has_sibling_combinator();

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

    auto& style_scope = node.style_scope();
    if (!style_scope.may_have_has_selectors() || !reason_may_affect_has_selectors(reason))
        return;
    style_scope.record_pending_has_invalidation_mutation_features(node, node, false);
    style_scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(node);
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
