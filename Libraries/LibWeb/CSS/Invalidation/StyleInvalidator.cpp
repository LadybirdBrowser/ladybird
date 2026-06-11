/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/Invalidation/InvalidationSetMatcher.h>
#include <LibWeb/CSS/Invalidation/StructuralMutationInvalidator.h>
#include <LibWeb/CSS/Invalidation/StyleInvalidator.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS::Invalidation {

GC_DEFINE_ALLOCATOR(StyleInvalidator);

static bool element_matches_invalidation_rule(DOM::Element const& element, CSS::InvalidationSet const& match_set, bool match_any)
{
    return match_any || element_matches_any_invalidation_set_property(element, match_set);
}

static bool element_matches_invalidation_guard(DOM::Element const& element, CSS::InvalidationGuard const& guard)
{
    for (auto const& property_set : guard.property_sets) {
        if (!element_matches_any_invalidation_set_property(element, property_set))
            return false;
    }
    return true;
}

static NonnullRefPtr<CSS::InvalidationPlan> resolve_guarded_invalidation_plan(DOM::Element const* element, CSS::InvalidationPlan const& plan)
{
    auto resolved_plan = CSS::InvalidationPlan::create();
    resolved_plan->invalidate_self = plan.invalidate_self;
    resolved_plan->invalidate_self_and_structurally_affected_siblings = plan.invalidate_self_and_structurally_affected_siblings;

    if (plan.invalidate_whole_subtree) {
        resolved_plan->invalidate_whole_subtree = true;
        resolved_plan->invalidate_self_and_structurally_affected_siblings = false;
        return resolved_plan;
    }

    resolved_plan->descendant_rules = plan.descendant_rules;
    resolved_plan->sibling_rules = plan.sibling_rules;

    if (!element)
        return resolved_plan;

    for (auto const& guarded_rule : plan.guarded_rules) {
        if (!element_matches_invalidation_guard(*element, guarded_rule.guard))
            continue;
        resolved_plan->include_all_from(*guarded_rule.payload);
    }
    return resolved_plan;
}

void StyleInvalidator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& it : m_pending_invalidations)
        visitor.visit(it.key);
}

void StyleInvalidator::invalidate(DOM::Node& node)
{
    perform_pending_style_invalidations(node, false);
    m_pending_invalidations.clear();
}

bool StyleInvalidator::enqueue_invalidation_plan(DOM::Node& node, DOM::StyleInvalidationReason reason, CSS::InvalidationPlan const& plan)
{
    RefPtr<CSS::InvalidationPlan> resolved_plan;
    auto* element = as_if<DOM::Element>(node);
    auto const& plan_to_apply = [&]() -> CSS::InvalidationPlan const& {
        if (plan.guarded_rules.is_empty())
            return plan;
        resolved_plan = resolve_guarded_invalidation_plan(element, plan);
        return *resolved_plan;
    }();

    if (plan_to_apply.is_empty())
        return false;

    if (plan_to_apply.invalidate_whole_subtree) {
        node.invalidate_style(reason);
        return true;
    }

    if (plan_to_apply.invalidate_self_and_structurally_affected_siblings)
        invalidate_self_and_structurally_affected_siblings(node, reason);

    if (plan_to_apply.invalidate_self)
        node.set_needs_style_update(true);

    add_pending_invalidation(node, reason, plan_to_apply);

    if (element) {
        for (auto const& sibling_rule : plan_to_apply.sibling_rules)
            apply_sibling_invalidation(*element, reason, sibling_rule);
    }

    return false;
}

void StyleInvalidator::add_pending_invalidation(GC::Ref<DOM::Node> node, DOM::StyleInvalidationReason reason, CSS::InvalidationPlan const& plan)
{
    if (plan.descendant_rules.is_empty())
        return;

    auto& pending_invalidations = m_pending_invalidations.ensure(node, [] {
        return Vector<PendingDescendantInvalidation> {};
    });
    for (auto const& descendant_rule : plan.descendant_rules) {
        PendingDescendantInvalidation pending_invalidation { reason, descendant_rule };
        if (!pending_invalidations.contains_slow(pending_invalidation))
            pending_invalidations.append(move(pending_invalidation));
    }
}

void StyleInvalidator::apply_invalidation_plan(DOM::Element& element, DOM::StyleInvalidationReason reason, CSS::InvalidationPlan const& plan, bool& invalidate_entire_subtree)
{
    RefPtr<CSS::InvalidationPlan> resolved_plan;
    auto const& plan_to_apply = [&]() -> CSS::InvalidationPlan const& {
        if (plan.guarded_rules.is_empty())
            return plan;
        resolved_plan = resolve_guarded_invalidation_plan(&element, plan);
        return *resolved_plan;
    }();

    if (plan_to_apply.is_empty())
        return;

    if (plan_to_apply.invalidate_whole_subtree) {
        element.invalidate_style(reason);
        invalidate_entire_subtree = true;
        element.set_needs_style_update_internal(true);
        if (element.has_child_nodes())
            element.set_child_needs_style_update(true);
        return;
    }

    if (plan_to_apply.invalidate_self_and_structurally_affected_siblings)
        invalidate_self_and_structurally_affected_siblings(element, reason);

    if (plan_to_apply.invalidate_self)
        element.set_needs_style_update(true);

    for (auto const& descendant_rule : plan_to_apply.descendant_rules) {
        PendingDescendantInvalidation pending_invalidation { reason, descendant_rule };
        if (!m_active_descendant_invalidations.contains_slow(pending_invalidation))
            m_active_descendant_invalidations.append(move(pending_invalidation));
    }

    for (auto const& sibling_rule : plan_to_apply.sibling_rules)
        apply_sibling_invalidation(element, reason, sibling_rule);
}

void StyleInvalidator::apply_sibling_invalidation(DOM::Element& element, DOM::StyleInvalidationReason reason, CSS::SiblingInvalidationRule const& sibling_rule)
{
    auto apply_if_matching = [&](DOM::Element* sibling) {
        if (!sibling)
            return;
        if (!element_matches_invalidation_rule(*sibling, sibling_rule.match_set, sibling_rule.match_any))
            return;
        (void)enqueue_invalidation_plan(*sibling, reason, *sibling_rule.payload);
    };

    switch (sibling_rule.reach) {
    case CSS::SiblingInvalidationReach::Adjacent:
        apply_if_matching(element.next_element_sibling());
        break;
    case CSS::SiblingInvalidationReach::Subsequent:
        for (auto* sibling = element.next_element_sibling(); sibling; sibling = sibling->next_element_sibling())
            apply_if_matching(sibling);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

// This function makes a full pass over the entire DOM and:
// - converts "entire subtree needs style update" into "needs style update" for each inclusive descendant where it's found.
// - applies descendant invalidation rules to matching elements
void StyleInvalidator::perform_pending_style_invalidations(DOM::Node& node, bool invalidate_entire_subtree)
{
    invalidate_entire_subtree |= node.entire_subtree_needs_style_update();
    auto* element = as_if<DOM::Element>(node);

    if (invalidate_entire_subtree) {
        node.set_needs_style_update_internal(true);
        if (node.has_child_nodes())
            node.set_child_needs_style_update(true);
    }

    auto previous_active_descendant_invalidations_size = m_active_descendant_invalidations.size();
    ScopeGuard restore_state = [this, previous_active_descendant_invalidations_size] {
        m_active_descendant_invalidations.shrink(previous_active_descendant_invalidations_size);
    };

    if (!invalidate_entire_subtree) {
        if (auto pending_invalidations = m_pending_invalidations.get(node); pending_invalidations.has_value()) {
            m_active_descendant_invalidations.extend(*pending_invalidations);
        }

        if (element) {
            size_t invalidation_index = 0;
            while (invalidation_index < m_active_descendant_invalidations.size()) {
                auto const& pending_invalidation = m_active_descendant_invalidations[invalidation_index++];
                if (!element_matches_invalidation_rule(*element, pending_invalidation.rule.match_set, pending_invalidation.rule.match_any))
                    continue;

                apply_invalidation_plan(*element, pending_invalidation.reason, *pending_invalidation.rule.payload, invalidate_entire_subtree);
                if (invalidate_entire_subtree)
                    break;
            }

            if (invalidate_entire_subtree) {
                node.set_needs_style_update_internal(true);
                if (node.has_child_nodes())
                    node.set_child_needs_style_update(true);
            }
        }
    }

    if (element)
        element->clear_removed_attributes_for_style_invalidation();

    for (auto* child = node.first_child(); child; child = child->next_sibling())
        perform_pending_style_invalidations(*child, invalidate_entire_subtree);

    if (node.is_element()) {
        auto& element = static_cast<DOM::Element&>(node);
        if (auto shadow_root = element.shadow_root()) {
            perform_pending_style_invalidations(*shadow_root, invalidate_entire_subtree);
            if (invalidate_entire_subtree || shadow_root->needs_style_update() || shadow_root->child_needs_style_update()) {
                for (auto* ancestor = &node; ancestor; ancestor = ancestor->parent_or_shadow_host())
                    ancestor->set_child_needs_style_update(true);
            }
        }
    }

    node.set_entire_subtree_needs_style_update(false);
}

}
