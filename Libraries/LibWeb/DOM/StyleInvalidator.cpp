/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleInvalidator.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(StyleInvalidator);

void StyleInvalidator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_pending_invalidations);
}

void StyleInvalidator::invalidate(Node& node)
{
    perform_pending_style_invalidations(node, false);
    m_pending_invalidations.clear();
}

void StyleInvalidator::add_pending_invalidation(GC::Ref<Node> node, CSS::InvalidationSet&& invalidation_set)
{
    auto& pending_invalidation_set = m_pending_invalidations.ensure(node, [&] {
        return CSS::InvalidationSet {};
    });
    pending_invalidation_set.include_all_from(invalidation_set);
}

// This function makes a full pass over the entire DOM and:
// - converts "entire subtree needs style update" into "needs style update" for each inclusive descendant where it's found.
// - marks nodes included into pending invalidation sets as "needs style update"
void StyleInvalidator::perform_pending_style_invalidations(Node& node, bool invalidate_entire_subtree)
{
    invalidate_entire_subtree |= node.entire_subtree_needs_style_update();

    if (invalidate_entire_subtree) {
        node.set_needs_style_update_internal(true);
        if (node.has_child_nodes())
            node.set_child_needs_style_update(true);
    }

    auto previous_subtree_invalidations_sets_size = m_subtree_invalidation_sets.size();
    ScopeGuard restore_state = [this, previous_subtree_invalidations_sets_size] {
        m_subtree_invalidation_sets.shrink(previous_subtree_invalidations_sets_size);
    };

    if (!invalidate_entire_subtree) {
        auto invalidation_set = m_pending_invalidations.get(node);
        if (invalidation_set.has_value()) {
            m_subtree_invalidation_sets.append(*invalidation_set);
        }

        auto affected_by_invalidation_sets_or_invalidation_flags = [this](Element const& element) {
            for (auto& invalidation_set : m_subtree_invalidation_sets) {
                if (element.includes_properties_from_invalidation_set(invalidation_set))
                    return true;
            }
            return false;
        };

        if (auto* element = as_if<Element>(node); element && affected_by_invalidation_sets_or_invalidation_flags(*element)) {
            node.set_needs_style_update(true);
        }
    }

    for (auto* child = node.first_child(); child; child = child->next_sibling()) {
        perform_pending_style_invalidations(*child, invalidate_entire_subtree);
    }

    if (node.is_element()) {
        auto& element = static_cast<Element&>(node);
        if (auto shadow_root = element.shadow_root()) {
            perform_pending_style_invalidations(*shadow_root, invalidate_entire_subtree);
            if (invalidate_entire_subtree)
                node.set_child_needs_style_update(true);
        }
    }

    node.set_entire_subtree_needs_style_update(false);
}

}
