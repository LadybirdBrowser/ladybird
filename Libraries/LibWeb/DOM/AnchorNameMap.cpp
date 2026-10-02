/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AnchorNameMap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::DOM {

void AnchorNameMap::register_name(Utf16FlyString const& name, GC::Ref<Element> element, Optional<CSS::StyleNodeID> scope_host)
{
    auto& elements = m_map.ensure(name);
    if (elements.contains_slow(element))
        return;

    // Insert in tree order so that .last() is always the last element in tree order.
    auto index = elements.find_first_index_if([&](auto& existing) {
        return element->is_before(existing);
    });
    if (index.has_value())
        elements.insert(*index, element);
    else
        elements.append(element);

    publish(name, element->document(), scope_host);
}

void AnchorNameMap::unregister_name(Utf16FlyString const& name, GC::Ref<Element> element, Optional<CSS::StyleNodeID> scope_host)
{
    auto it = m_map.find(name);
    if (it == m_map.end())
        return;
    it->value.remove_first_matching([&](auto& e) { return e == element; });
    if (it->value.is_empty())
        m_map.remove(it);
    publish(name, element->document(), scope_host);
}

void AnchorNameMap::publish(Utf16FlyString const& name, Document& document, Optional<CSS::StyleNodeID> scope_host) const
{
    // A shadow host whose identity is already retired has left the tree with its shadow root, and the
    // arena forgot the scope with the identity. Naming it anyway would name the document tree.
    if (scope_host.has_value() && !scope_host->value())
        return;

    auto it = m_map.find(name);
    auto* arena = document.layout_node_arena_if_created();
    if (!arena) {
        // Nothing was published to a document without an arena, so there is nothing to withdraw. A
        // name to publish creates it: a document that registers a name is one that lays out.
        if (it == m_map.end())
            return;
        arena = &document.layout_node_arena();
    }

    Vector<u32, 4> style_nodes;
    if (it != m_map.end()) {
        style_nodes.ensure_capacity(it->value.size());
        for (auto const& element : it->value)
            style_nodes.unchecked_append(element->style_node_id().value());
    }
    Layout::RustFFI::render_state_set_anchor_name_elements(
        arena->host(),
        scope_host.value_or(CSS::StyleNodeID {}).value(),
        name.raw_identity(),
        style_nodes.data(),
        style_nodes.size());
}

}
