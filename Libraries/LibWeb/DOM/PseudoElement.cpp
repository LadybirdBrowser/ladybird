/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(PseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElementTreeNode);
GC_DEFINE_ALLOCATOR(ElementReferencePseudoElement);

struct SyntheticPseudoElement::CustomPropertyDataStorage {
    RefPtr<CSS::CustomPropertyData const> data;
};

SyntheticPseudoElement::SyntheticPseudoElement() = default;
SyntheticPseudoElement::~SyntheticPseudoElement() = default;

void SyntheticPseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    if (m_counters_set)
        m_counters_set->visit_edges(visitor);
}

void SyntheticPseudoElement::set_layout_node(Layout::NodeWithStyle* value)
{
    m_layout_node = value;
}

RefPtr<CSS::ComputedProperties const> SyntheticPseudoElement::computed_properties() const
{
    return m_computed_properties;
}

void SyntheticPseudoElement::update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement abstract_element, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    if (!m_computed_properties)
        return;
    effect.update_computed_properties_for_style(context, abstract_element, *m_computed_properties);
}

void SyntheticPseudoElement::set_computed_properties(RefPtr<CSS::ComputedProperties> value)
{
    m_computed_properties = value;
}

RefPtr<CSS::CustomPropertyData const> SyntheticPseudoElement::custom_property_data() const
{
    if (!m_custom_property_data)
        return nullptr;
    return m_custom_property_data->data;
}

void SyntheticPseudoElement::set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value)
{
    if (!value) {
        m_custom_property_data = nullptr;
        return;
    }

    if (!m_custom_property_data)
        m_custom_property_data = make<CustomPropertyDataStorage>();
    m_custom_property_data->data = move(value);
}

Optional<CSS::CountersSet const&> SyntheticPseudoElement::counters_set() const
{
    if (!m_counters_set)
        return {};
    return *m_counters_set;
}

CSS::CountersSet& SyntheticPseudoElement::ensure_counters_set()
{
    if (!m_counters_set)
        m_counters_set = make<CSS::CountersSet>();
    return *m_counters_set;
}

void SyntheticPseudoElement::set_counters_set(OwnPtr<CSS::CountersSet>&& counters_set)
{
    m_counters_set = move(counters_set);
}

SyntheticPseudoElementTreeNode::SyntheticPseudoElementTreeNode() = default;
SyntheticPseudoElementTreeNode::~SyntheticPseudoElementTreeNode() = default;

void SyntheticPseudoElementTreeNode::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    TreeNode::visit_edges(visitor);
}

Layout::NodeWithStyle* ElementReferencePseudoElement::layout_node() const
{
    return m_referenced_element->layout_node();
}

Layout::NodeWithStyle* ElementReferencePseudoElement::unsafe_layout_node() const
{
    return m_referenced_element->unsafe_layout_node();
}

RefPtr<CSS::ComputedProperties const> ElementReferencePseudoElement::computed_properties() const
{
    return m_referenced_element->computed_properties({});
}

void ElementReferencePseudoElement::update_animated_properties(Badge<Web::Animations::KeyframeEffect> const& badge, DOM::AbstractElement abstract_element, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    m_referenced_element->update_animated_properties_for_abstract_element(badge, abstract_element, effect, context);
}

RefPtr<CSS::CustomPropertyData const> ElementReferencePseudoElement::custom_property_data() const
{
    return m_referenced_element->custom_property_data({});
}

void ElementReferencePseudoElement::set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value)
{
    m_referenced_element->set_custom_property_data({}, move(value));
}

void ElementReferencePseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_referenced_element);
}

}
