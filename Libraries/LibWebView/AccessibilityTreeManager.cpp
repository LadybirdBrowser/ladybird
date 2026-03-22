/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/AccessibilityTreeManager.h>

namespace WebView {

void AccessibilityTreeManager::update_tree(Vector<AccessibilityNodeData> nodes)
{
    // Detect live region content changes before replacing the tree.
    if (on_live_region_changed && !m_nodes.is_empty()) {
        for (auto const& new_node : nodes) {
            // Check if this node is inside a live region.
            // Walk up ancestors in the NEW tree to find a live region.
            String live_value;
            for (auto const* ancestor = &new_node; ancestor;) {
                if (!ancestor->live.is_empty()) {
                    live_value = ancestor->live;
                    break;
                }
                if (ancestor->parent_id == -1)
                    break;
                // Find parent in new nodes list
                bool found = false;
                for (auto const& n : nodes) {
                    if (n.id == ancestor->parent_id) {
                        ancestor = &n;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    break;
            }

            if (live_value.is_empty() || live_value == "off"sv)
                continue;

            // Compare with old node
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
