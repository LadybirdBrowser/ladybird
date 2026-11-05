/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibWebCommon/WebView/AccessibilityTreePublisher.h>

namespace WebView {

AccessibilityTreePublisher::Update AccessibilityTreePublisher::publish(i64 document_id, Vector<AccessibilityNodeData> nodes, bool whole_tree_requested)
{
    auto remember = [&](Vector<AccessibilityNodeData> const& published) {
        m_document_id = document_id;
        m_published_nodes.clear();
        m_published_nodes.ensure_capacity(published.size());
        for (auto const& node : published)
            m_published_nodes.set(node.id, node);
    };

    if (whole_tree_requested || m_document_id != document_id) {
        remember(nodes);
        return WholeTree { ++m_sequence, move(nodes) };
    }

    // Every node that's new or whose data changed goes in the change, in the order the tree lists them. A node whose
    // children changed is among them, so a node that moved is listed by its new parent, and dropped by its old one.
    // The tree sent keeps each node's new data, moved from the tree just built rather than copied again.
    AccessibilityTreeDelta delta;
    Vector<size_t> new_node_indices;
    size_t surviving_count = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        auto published = m_published_nodes.find(node.id);
        if (published == m_published_nodes.end()) {
            delta.changed_nodes.append(node);
            new_node_indices.append(i);
            continue;
        }
        ++surviving_count;
        if (published->value == node)
            continue;
        delta.changed_nodes.append(node);
        published->value = move(node);
    }

    // Only when fewer nodes of the tree sent survive than it had are any of them gone. (A node moved from still has its
    // ID: Moving an integer copies it.)
    if (surviving_count < m_published_nodes.size()) {
        HashTable<i64> current_ids;
        current_ids.ensure_capacity(nodes.size());
        for (auto const& node : nodes)
            current_ids.set(node.id);
        for (auto const& [id, node] : m_published_nodes) {
            if (!current_ids.contains(id))
                delta.removed_ids.append(id);
        }
        for (auto id : delta.removed_ids)
            m_published_nodes.remove(id);
    }
    for (auto i : new_node_indices)
        m_published_nodes.set(nodes[i].id, move(nodes[i]));

    if (delta.changed_nodes.is_empty() && delta.removed_ids.is_empty())
        return NothingChanged {};

    auto base_sequence = m_sequence;
    return Change { base_sequence, ++m_sequence, move(delta) };
}

}
