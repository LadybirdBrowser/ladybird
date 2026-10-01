/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::CSS {

static void* layout_node_arena_handle(DOM::Element const& element)
{
    auto* arena = element.document().layout_node_arena_if_created();
    return arena ? const_cast<Layout::NodeArena*>(arena)->handle() : nullptr;
}

static u8 generated_for(DOM::AbstractElement const& element)
{
    auto pseudo_element = element.pseudo_element();
    return pseudo_element.has_value() ? Layout::Node::encode_generated_for(*pseudo_element) : 0;
}

// https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
// "If there is not currently a counter of the given name on the element, the element instantiates
// a new counter of the given name with a starting value of 0 before setting or incrementing its value."
CounterValue counter_value_for_use(DOM::AbstractElement const& element, Utf16FlyString const& name)
{
    auto* arena = layout_node_arena_handle(element.element());
    if (!arena)
        return 0;
    return Layout::RustFFI::layout_arena_counter_value_for_use(arena, element.element().style_node_id().value(), generated_for(element), name.raw_identity());
}

Vector<CounterValue> counter_values_for_use(DOM::AbstractElement const& element, Utf16FlyString const& name)
{
    Vector<CounterValue> values;
    auto* arena = layout_node_arena_handle(element.element());
    if (!arena) {
        values.append(0);
        return values;
    }
    Layout::RustFFI::layout_arena_counter_values_for_use(arena, element.element().style_node_id().value(), generated_for(element), name.raw_identity(), &values, [](void* context, i32 value) {
        static_cast<Vector<CounterValue>*>(context)->append(value);
    });
    return values;
}

bool innermost_list_item_counter_is_own_forward_counter(DOM::Element const& element)
{
    auto* arena = layout_node_arena_handle(element);
    if (!arena)
        return false;
    return Layout::RustFFI::layout_arena_innermost_list_item_counter_is_own_forward_counter(arena, element.style_node_id().value());
}

Utf16FlyString const& list_item_counter_name()
{
    static NeverDestroyed<Utf16FlyString> name = "list-item"_utf16_fly_string;
    return *name;
}

}
