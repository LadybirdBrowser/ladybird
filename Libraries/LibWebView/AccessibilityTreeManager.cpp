/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/AccessibilityTreeManager.h>

namespace WebView {

void AccessibilityTreeManager::update_tree(Vector<AccessibilityNodeData> nodes)
{
    // Detect live-region content changes before replacing the tree. Needs an index of the new nodes by id — so the
    // ancestor walk below is O(depth) rather than O(N) per step. Without this, a full traversal of an N-node tree would
    // be O(N^2) on every tree update.
    if (on_live_region_changed && !m_nodes.is_empty()) {
        HashMap<i64, AccessibilityNodeData const*> new_nodes_by_id;
        new_nodes_by_id.ensure_capacity(nodes.size());
        for (auto const& node : nodes)
            new_nodes_by_id.set(node.id, &node);

        for (auto const& new_node : nodes) {
            // Walk up ancestors in the new tree looking for an enclosing live region.
            String live_value;
            for (auto const* ancestor = &new_node; ancestor;) {
                if (!ancestor->live.is_empty()) {
                    live_value = ancestor->live;
                    break;
                }
                if (ancestor->parent_id == -1)
                    break;
                auto parent_entry = new_nodes_by_id.get(ancestor->parent_id);
                if (!parent_entry.has_value())
                    break;
                ancestor = *parent_entry;
            }

            if (live_value.is_empty() || live_value == "off"sv)
                continue;

            auto old_it = m_nodes.find(new_node.id);
            if (old_it == m_nodes.end())
                continue;

            auto const& old_node = old_it->value;
            if (old_node.name != new_node.name && !new_node.name.is_empty())
                on_live_region_changed(new_node.name, live_value);
        }
    }

    m_nodes.clear();
    m_root_id = -1;
    ++m_generation;

    for (auto& node : nodes) {
        auto id = node.id;
        if (node.parent_id == -1)
            m_root_id = id;
        m_nodes.set(id, move(node));
    }
}

AccessibilityNodeData const* AccessibilityTreeManager::node(i64 id) const
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end())
        return nullptr;
    return &it->value;
}

AccessibilityNodeData const* AccessibilityTreeManager::root() const
{
    if (m_root_id == -1)
        return nullptr;
    return node(m_root_id);
}

Vector<i64> AccessibilityTreeManager::text_leaves_in_order() const
{
    Vector<i64> result;
    if (m_root_id == -1)
        return result;

    // DFS pre-order traversal collecting text leaf nodes
    Vector<i64> stack;
    stack.append(m_root_id);

    while (!stack.is_empty()) {
        auto id = stack.take_last();
        auto const* n = node(id);
        if (!n)
            continue;
        if (n->role.bytes_as_string_view() == "text leaf"sv && !n->name.is_empty())
            result.append(id);
        // Push children in reverse for correct DFS order
        for (int i = static_cast<int>(n->child_ids.size()) - 1; i >= 0; --i)
            stack.append(n->child_ids[i]);
    }

    return result;
}

AccessibilityNodeData const* AccessibilityTreeManager::hit_test(Gfx::IntPoint point) const
{
    if (m_root_id == -1)
        return nullptr;
    return hit_test_recursive(m_root_id, point);
}

void AccessibilityTreeManager::set_focused_node(i64 node_id)
{
    for (auto& [id, node] : m_nodes)
        node.is_focused = (id == node_id);
}

Optional<i64> AccessibilityTreeManager::focused_node_id() const
{
    for (auto const& [id, node] : m_nodes) {
        if (node.is_focused && id != m_root_id)
            return id;
    }
    return {};
}

AccessibilityNodeData const* AccessibilityTreeManager::hit_test_recursive(i64 node_id, Gfx::IntPoint point) const
{
    auto const* current = node(node_id);
    if (!current)
        return nullptr;

    // Walk children in reverse order (last painted = on top) to find the deepest hit.
    for (int i = static_cast<int>(current->child_ids.size()) - 1; i >= 0; --i) {
        auto const* result = hit_test_recursive(current->child_ids[i], point);
        if (result)
            return result;
    }

    if (current->bounds.contains(point))
        return current;

    return nullptr;
}

}
