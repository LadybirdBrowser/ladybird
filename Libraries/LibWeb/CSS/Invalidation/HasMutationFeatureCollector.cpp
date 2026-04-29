/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/HasMutationFeatureCollector.h>
#include <LibWeb/CSS/Invalidation/InvalidationSetMatcher.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/StyleInvalidationData.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Namespace.h>

namespace Web::CSS::Invalidation {

HasMutationFeatureCollector::HasMutationFeatureCollector(StyleInvalidationData const& data)
    : m_data(data)
{
}

bool HasMutationFeatureCollector::has_any_metadata() const
{
    return !m_data.ids_used_in_has_selectors.is_empty()
        || !m_data.class_names_used_in_has_selectors.is_empty()
        || !m_data.attribute_names_used_in_has_selectors.is_empty()
        || !m_data.tag_names_used_in_has_selectors.is_empty()
        || !m_data.pseudo_classes_used_in_has_selectors.is_empty()
        || m_data.has_selectors_sensitive_to_featureless_subtree_changes;
}

bool HasMutationFeatureCollector::element_has_feature_used_in_has_selector(DOM::Element const& element) const
{
    if (m_data.tag_names_used_in_has_selectors.contains(element.local_name()))
        return true;
    // Selector metadata stores tag and attribute names lowercased. For non-HTML elements this is only a conservative
    // scheduling hint; the actual :has() match still uses selector matching with the element's namespace semantics.
    if (element.namespace_uri() != Namespace::HTML
        && m_data.tag_names_used_in_has_selectors.contains(element.lowercased_local_name()))
        return true;
    if (auto id = element.id(); id.has_value() && m_data.ids_used_in_has_selectors.contains(*id))
        return true;
    for (auto const& class_name : element.class_names()) {
        if (m_data.class_names_used_in_has_selectors.contains(class_name))
            return true;
    }
    bool attribute_used_in_has = false;
    element.for_each_attribute([&](FlyString const& name, String const&) {
        if (m_data.attribute_names_used_in_has_selectors.contains(name))
            attribute_used_in_has = true;
        if (element.namespace_uri() != Namespace::HTML
            && m_data.attribute_names_used_in_has_selectors.contains(name.to_ascii_lowercase()))
            attribute_used_in_has = true;
    });
    if (attribute_used_in_has)
        return true;
    for (auto const& pseudo_class_entry : m_data.pseudo_classes_used_in_has_selectors) {
        CSS::InvalidationSet pseudo_class_set;
        pseudo_class_set.set_needs_invalidate_pseudo_class(pseudo_class_entry.key);
        if (element_matches_any_invalidation_set_property(element, pseudo_class_set))
            return true;
    }
    return false;
}

bool HasMutationFeatureCollector::subtree_has_feature_used_in_has_selector(DOM::Node& node) const
{
    // Some :has() arguments can match because a node exists, stops existing, or changes position, even when that
    // node has no tag/class/id/attribute/pseudo-class metadata we can compare against. Examples include :has(*),
    // :has(:not(.x)), :has(:empty), and child-index pseudo-classes. Keep the old conservative walk for those.
    if (m_data.has_selectors_sensitive_to_featureless_subtree_changes)
        return true;

    bool found = false;
    node.for_each_in_inclusive_subtree([&](DOM::Node& node) {
        if (node.is_character_data()) {
            found = true;
            return TraversalDecision::Break;
        }
        if (auto* element = as_if<DOM::Element>(node); element && element_has_feature_used_in_has_selector(*element)) {
            found = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return found;
}

bool subtree_has_feature_used_in_has_selector(DOM::Node& node, StyleScope const& style_scope)
{
    auto const* data = style_scope.m_rule_cache ? &style_scope.m_rule_cache->style_invalidation_data : nullptr;
    if (!data)
        return true;

    HasMutationFeatureCollector collector { *data };
    if (!collector.has_any_metadata())
        return true;

    return collector.subtree_has_feature_used_in_has_selector(node);
}

}
