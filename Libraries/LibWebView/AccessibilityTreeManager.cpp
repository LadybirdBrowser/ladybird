/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/AccessibilityTreeManager.h>

namespace WebView {

void AccessibilityTreeManager::update_tree(Vector<AccessibilityNodeData> nodes)
{
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
