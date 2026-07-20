/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/InsertedContent.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/ReplacementStyleCleanup.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLSpanElement.h>

namespace Web::Editing {

static bool computed_property_values_are_equivalent(CSS::PropertyID property_id, CSS::ComputedProperties const& with_inline_style, CSS::ComputedProperties const& without_inline_style)
{
    auto const& value_with_inline_style = with_inline_style.property(property_id);
    auto const& value_without_inline_style = without_inline_style.property(property_id);
    if (value_with_inline_style == value_without_inline_style)
        return true;

    // Computed values can retain different internal representations after reconstruction even when their canonical
    // CSS serialization is identical. Editing style comparison treats those representations as equivalent.
    if (value_with_inline_style.to_string(CSS::SerializationMode::Normal) == value_without_inline_style.to_string(CSS::SerializationMode::Normal))
        return true;

    return false;
}

void remove_redundant_styles_from_inserted_content(InsertedContent& inserted_content)
{
    GC::RootVector<GC::Ref<DOM::Element>> styled_elements;
    for (auto& node : inserted_content.nodes()) {
        node->for_each_in_inclusive_subtree([&](GC::Ref<DOM::Node> descendant) {
            auto* element = as_if<DOM::Element>(*descendant);
            if (element && element->has_attribute(HTML::AttributeNames::style))
                styled_elements.append(*element);
            return TraversalDecision::Continue;
        });
    }

    // INTEROP: Blink and WebKit remove styles supplied by matched rules and the insertion context after inserting the
    //          fragment. Comparing the two cascades directly gives us the same property-level decision without making
    //          speculative live DOM mutations, which would be observable and would create bogus undo commands.
    for (auto& element : styled_elements) {
        if (!element->parent())
            continue;

        auto inline_style = element->inline_style();
        if (!inline_style)
            continue;

        element->document().update_layout_if_needed_for_node(element, DOM::UpdateLayoutReason::NavigableSelectedText);
        DOM::AbstractElement abstract_element { element };
        auto const* computed_values = abstract_element.computed_values();
        if (!computed_values)
            continue;

        auto& style_computer = element->document().style_computer();
        auto style_with_inline_declaration = style_computer.reconstruct_computed_properties(*computed_values);
        auto style_without_inline_declaration = style_computer.compute_properties_without_inline_style(abstract_element);

        Vector<CSS::StyleProperty> retained_properties;
        retained_properties.ensure_capacity(inline_style->properties().size());
        for (auto const& property : inline_style->properties()) {
            if (!computed_property_values_are_equivalent(property.property_id, *style_with_inline_declaration, *style_without_inline_declaration))
                retained_properties.append(property);
        }

        if (retained_properties.size() == inline_style->properties().size())
            continue;

        auto retained_style = CSS::CSSStyleProperties::create(element->realm(), move(retained_properties), inline_style->custom_properties());
        auto serialized_style = retained_style->serialized();
        if (!serialized_style.is_empty()) {
            set_attribute_value(element, HTML::AttributeNames::style, serialized_style);
            continue;
        }

        auto should_unwrap = is<HTML::HTMLSpanElement>(*element) && element->attribute_list_size() == 1 && element->has_children();
        remove_attribute(element, HTML::AttributeNames::style);
        if (!should_unwrap)
            continue;

        auto first_child = GC::Ref { *element->first_child() };
        auto last_child = GC::Ref { *element->last_child() };
        unwrap_node_preserving_ranges(element);
        inserted_content.did_replace_node(element, first_child, last_child);
    }
}

}
