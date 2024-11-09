/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/NodeOperations.h>
#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#converting-nodes-into-a-node
WebIDL::ExceptionOr<JS::NonnullGCPtr<Node>> convert_nodes_to_single_node(Vector<Variant<JS::Handle<Node>, String>> const& nodes, DOM::Document& document)
{
    // 1. Let node be null.
    // 2. Replace each string in nodes with a new Text node whose data is the string and node document is document.
    // 3. If nodes contains one node, then set node to nodes[0].
    // 4. Otherwise, set node to a new DocumentFragment node whose node document is document, and then append each node in nodes, if any, to it.
    // 5. Return node.

    auto potentially_convert_string_to_text_node = [&document](Variant<JS::Handle<Node>, String> const& node) -> JS::NonnullGCPtr<Node> {
        if (node.has<JS::Handle<Node>>())
            return *node.get<JS::Handle<Node>>();

        return document.heap().allocate<DOM::Text>(document.realm(), document, node.get<String>());
    };

    if (nodes.size() == 1)
        return potentially_convert_string_to_text_node(nodes.first());

    auto document_fragment = document.heap().allocate<DOM::DocumentFragment>(document.realm(), document);
    for (auto const& unconverted_node : nodes) {
        auto node = potentially_convert_string_to_text_node(unconverted_node);
        (void)TRY(document_fragment->append_child(node));
    }

    return document_fragment;
}

}
