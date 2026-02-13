/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
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
    visitor.visit(m_inheritance_override);
}

Document& AbstractElement::document() const
{
    return m_element->document();
}

AbstractElement::TreeCountingFunctionResolutionContext AbstractElement::tree_counting_function_resolution_context() const
{
    // FIXME: When used on an element-backed pseudo-element which is also a real element, the tree counting functions
    //        resolve for that real element. For other pseudo elements, they resolve as if they were resolved against
    //        the originating element. It follows that for nested pseudo elements the resolution will recursively walk
    //        the originating elements until a real element is found.

    // FIXME: A tree counting function is a tree-scoped reference where it references an implicit tree-scoped name for
    //        the element it resolves against. This is done to not leak tree information to an outer tree. A tree
    //        counting function that is scoped to an outer tree relative to the element it resolves against, will alway
    //        resolve to 0.
    auto const& element_to_resolve_tree_counting_function_against = element();

    // The sibling-count() functional notation represents, as an <integer>, the total number of child elements in the
    // parent of the element on which the notation is used.
    auto const& parent = element_to_resolve_tree_counting_function_against.parent_element();

    // If there is no parent we are the root node
    if (!parent)
        return { .sibling_count = 1, .sibling_index = 1 };

    size_t count = 0;
    size_t index = 0;

    for (auto const* child = parent->first_child_of_type<DOM::Element>(); child; child = child->next_element_sibling()) {
        ++count;
        if (child == &element_to_resolve_tree_counting_function_against)
            index = count;
    }

    return {
        .sibling_count = count,
        .sibling_index = index
    };
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

Optional<AbstractElement> AbstractElement::element_to_inherit_style_from() const
{
    if (m_inheritance_override)
        return AbstractElement { *m_inheritance_override };

    GC::Ptr<Element const> element = m_element->element_to_inherit_style_from(m_pseudo_element);

    if (!element)
        return OptionalNone {};

    return AbstractElement { const_cast<DOM::Element&>(*element) };
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

        if (node->is_generated_for_pseudo_element())
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
    return m_element->computed_properties(m_pseudo_element);
}

RefPtr<CSS::CustomPropertyData const> AbstractElement::custom_property_data() const
{
    return m_element->custom_property_data(m_pseudo_element);
}

void AbstractElement::set_custom_property_data(RefPtr<CSS::CustomPropertyData const> data)
{
    m_element->set_custom_property_data(m_pseudo_element, move(data));
}

RefPtr<CSS::StyleValue const> AbstractElement::get_custom_property(FlyString const& name) const
{
    auto data = custom_property_data();
    if (!data)
        return nullptr;
    if (auto const* property = data->get(name))
        return property->value;
    return nullptr;
}

GC::Ptr<CSS::CascadedProperties> AbstractElement::cascaded_properties() const
{
    return m_element->cascaded_properties(m_pseudo_element);
}

void AbstractElement::set_cascaded_properties(GC::Ptr<CSS::CascadedProperties> cascaded_properties)
{
    m_element->set_cascaded_properties(m_pseudo_element, cascaded_properties);
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

CSS::StyleScope const& AbstractElement::style_scope() const
{
    auto& root = m_element->root();
    if (root.is_shadow_root())
        return as<DOM::ShadowRoot>(root).style_scope();
    return root.document().style_scope();
}

HashMap<FlyString, GC::Ref<CSS::CSSAnimation>>* AbstractElement::css_defined_animations() const
{
    return m_element->css_defined_animations(m_pseudo_element);
}

void AbstractElement::set_has_css_defined_animations()
{
    m_element->set_has_css_defined_animations();
}

}
