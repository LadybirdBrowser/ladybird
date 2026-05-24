/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(PseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElementTreeNode);
GC_DEFINE_ALLOCATOR(ElementReferencePseudoElement);

void SyntheticPseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_computed_properties);
    visitor.visit(m_layout_node);
    if (m_counters_set)
        m_counters_set->visit_edges(visitor);
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

void SyntheticPseudoElementTreeNode::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    TreeNode::visit_edges(visitor);
}

GC::Ptr<Layout::NodeWithStyle> ElementReferencePseudoElement::layout_node() const
{
    return m_referenced_element->layout_node();
}

GC::Ptr<Layout::NodeWithStyle> ElementReferencePseudoElement::unsafe_layout_node() const
{
    return m_referenced_element->unsafe_layout_node();
}

GC::Ptr<CSS::ComputedProperties> ElementReferencePseudoElement::computed_properties() const
{
    return m_referenced_element->computed_properties({});
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
