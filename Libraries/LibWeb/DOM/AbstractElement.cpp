/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>

namespace Web::DOM {

AbstractElement::AbstractElement(GC::Ref<Element> element, Optional<CSS::PseudoElement> pseudo_element)
    : m_element(element)
    , m_pseudo_element(move(pseudo_element))
{
}

void AbstractElement::visit(GC::Cell::Visitor& visitor) const
{
    visitor.visit(m_element);
}

GC::Ptr<Layout::NodeWithStyle> AbstractElement::layout_node()
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element_node(*m_pseudo_element);
    return m_element->layout_node();
}

GC::Ptr<Element const> AbstractElement::parent_element() const
{
    if (m_pseudo_element.has_value())
        return m_element;
    return m_element->parent_element();
}

Optional<AbstractElement> AbstractElement::walk_layout_tree(WalkMethod walk_method)
{
    GC::Ptr<Layout::Node> node = layout_node();
    if (!node)
        return OptionalNone {};

    while (true) {
        switch (walk_method) {
        case WalkMethod::Previous:
            node = node->previous_in_pre_order();
            break;
        case WalkMethod::PreviousSibling:
            node = node->previous_sibling();
            break;
        }
        if (!node)
            return OptionalNone {};

        if (auto* previous_element = as_if<Element>(node->dom_node()))
            return AbstractElement { *previous_element };

        if (node->is_generated())
            return AbstractElement { *node->pseudo_element_generator(), node->generated_for_pseudo_element() };
    }
}

bool AbstractElement::is_before(AbstractElement const& other) const
{
    auto this_node = layout_node();
    auto other_node = other.layout_node();
    return this_node && other_node && this_node->is_before(*other_node);
}

GC::Ptr<CSS::ComputedProperties const> AbstractElement::computed_properties() const
{
    if (m_pseudo_element.has_value())
        return m_element->pseudo_element_computed_properties(*m_pseudo_element);
    return m_element->computed_properties();
}

bool AbstractElement::has_non_empty_counters_set() const
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element(*m_pseudo_element)->has_non_empty_counters_set();
    return m_element->has_non_empty_counters_set();
}

Optional<CSS::CountersSet const&> AbstractElement::counters_set() const
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element(*m_pseudo_element)->counters_set();
    return m_element->counters_set();
}

CSS::CountersSet& AbstractElement::ensure_counters_set()
{
    if (m_pseudo_element.has_value())
        return m_element->get_pseudo_element(*m_pseudo_element)->ensure_counters_set();
    return m_element->ensure_counters_set();
}

void AbstractElement::set_counters_set(OwnPtr<CSS::CountersSet>&& counters_set)
{
    if (m_pseudo_element.has_value()) {
        m_element->get_pseudo_element(*m_pseudo_element)->set_counters_set(move(counters_set));
    } else {
        m_element->set_counters_set(move(counters_set));
    }
}

String AbstractElement::debug_description() const
{
    if (m_pseudo_element.has_value()) {
        StringBuilder builder;
        builder.append(m_element->debug_description());
        builder.append("::"sv);
        builder.append(CSS::pseudo_element_name(*m_pseudo_element));
        return builder.to_string_without_validation();
    }
    return m_element->debug_description();
}

}
