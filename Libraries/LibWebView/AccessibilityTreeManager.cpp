/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibWebView/AccessibilityTreeManager.h>

namespace WebView {

namespace {

// WebContent is a separate, potentially-compromised process, so the parent_id/child_ids links in the accessibility
// node data it sends are untrusted. Left as-is, a cycle, a node reachable from two parents, a dangling child id,
// multiple roots, or an extremely deep chain would hang or overflow the stack of this (the UI) process during the tree
// traversals below and in the platform accessibility bridges. sanitize_tree() rebuilds the incoming data into a plain
// tree: a single root, every node reachable from it at most once (so no cycles and no shared children), each node's
// parent_id and child_ids kept consistent, and depth bounded — so every consumer can traverse it without needing
// its own cycle or depth guards.
//
// Legitimate accessibility trees are nowhere near this deep; the cap exists only to keep a hostile or buggy WebContent
// from overflowing the UI process stack.
constexpr size_t max_accessibility_tree_depth = 1000;

Vector<AccessibilityNodeData> sanitize_tree(Vector<AccessibilityNodeData> nodes, i64& out_root_id)
{
    out_root_id = -1;

    // Index nodes by id, keeping the first occurrence of any duplicated id, and take the first node that claims to be
    // the root (parent_id == -1) as the single root.
    HashMap<i64, size_t> index_by_id;
    index_by_id.ensure_capacity(nodes.size());
    i64 root_id = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto const& node = nodes[i];
        if (index_by_id.contains(node.id))
            continue;
        index_by_id.set(node.id, i);
        if (node.parent_id == -1 && root_id == -1)
            root_id = node.id;
    }
    if (root_id == -1)
        return {};

    // Breadth-first walk from the root over child_ids. `visited` accepts every node at most once, which breaks cycles
    // and rejects a node that appears under more than one parent; the depth cap bounds how deep the resulting tree can
    // get. accepted_children[id] is the pruned, order-preserving child list actually kept for that node.
    HashMap<i64, Vector<i64>> accepted_children;
    HashMap<i64, i64> parent_of;
    HashTable<i64> visited;

    struct QueueEntry {
        i64 id { 0 };
        size_t depth { 0 };
    };
    Vector<QueueEntry> queue;
    visited.set(root_id);
    parent_of.set(root_id, -1);
    queue.append({ root_id, 0 });

    for (size_t head = 0; head < queue.size(); ++head) {
        auto [id, depth] = queue[head];
        auto const& node = nodes[index_by_id.get(id).value()];

        Vector<i64> kept_children;
        if (depth < max_accessibility_tree_depth) {
            for (auto child_id : node.child_ids) {
                if (!index_by_id.contains(child_id))
                    continue; // dangling reference to a node that was not sent
                if (visited.contains(child_id))
                    continue; // already placed elsewhere: a cycle, a self-loop, or a second parent
                visited.set(child_id);
                parent_of.set(child_id, id);
                kept_children.append(child_id);
                queue.append({ child_id, depth + 1 });
            }
        }
        accepted_children.set(id, move(kept_children));
    }

    // Emit the reachable nodes in their original order, with parent_id and child_ids replaced by the sanitized links.
    // Nodes not reachable from the root (orphans, cycle remnants, depth-capped subtrees) are dropped.
    Vector<AccessibilityNodeData> result;
    result.ensure_capacity(visited.size());
    HashTable<i64> emitted;
    for (auto& node : nodes) {
        if (!visited.contains(node.id) || emitted.contains(node.id))
            continue;
        emitted.set(node.id);
        node.parent_id = parent_of.get(node.id).value();
        node.child_ids = move(accepted_children.find(node.id)->value);
        result.append(move(node));
    }

    out_root_id = root_id;
    return result;
}

}

void AccessibilityTreeManager::update_tree(Vector<AccessibilityNodeData> nodes)
{
    i64 root_id = -1;
    auto sanitized = sanitize_tree(move(nodes), root_id);

    // Detect live-region content changes before replacing the tree. Needs an index of the new nodes by id — so the
    // ancestor walk below is O(depth) rather than O(N) per step. Without this, a full traversal of an N-node tree would
    // be O(N^2) on every tree update. The parent_id links are safe to walk here because sanitize_tree() has already
    // made the tree acyclic and bounded.
    if (on_live_region_changed && !m_nodes.is_empty()) {
        HashMap<i64, AccessibilityNodeData const*> new_nodes_by_id;
        new_nodes_by_id.ensure_capacity(sanitized.size());
        for (auto const& node : sanitized)
            new_nodes_by_id.set(node.id, &node);

        for (auto const& new_node : sanitized) {
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
    m_root_id = root_id;
    ++m_generation;

    for (auto& node : sanitized)
        m_nodes.set(node.id, move(node));
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
