/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/NodeOperations.h>
#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#convert-nodes-into-a-node
WebIDL::ExceptionOr<GC::Ref<Node>> convert_nodes_to_single_node(Vector<Variant<GC::Root<Node>, Utf16String>> const& nodes, Document& document)
{
    // 1. Replace each string of nodes with a new Text node whose data is the string and node document is document.
    auto potentially_convert_string_to_text_node = [&document](Variant<GC::Root<Node>, Utf16String> const& node) -> GC::Ref<Node> {
        if (node.has<GC::Root<Node>>())
            return *node.get<GC::Root<Node>>();

        return document.realm().create<Text>(document, node.get<Utf16String>());
    };

    // 2. If nodes’s size is 1, then return nodes[0].
    if (nodes.size() == 1)
        return potentially_convert_string_to_text_node(nodes.first());

    // 3. Let fragment be a new DocumentFragment node whose node document is document.
    auto fragment = document.realm().create<DocumentFragment>(document);

    // 4. For each node of nodes: append node to fragment.
    for (auto const& unconverted_node : nodes) {
        auto node = potentially_convert_string_to_text_node(unconverted_node);
        TRY(fragment->append_child(node));
    }

    // 5. Return fragment.
    return fragment;
}

}
