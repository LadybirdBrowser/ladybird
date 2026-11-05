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

Vector<AccessibilityNodeData> sanitize_tree(Vector<AccessibilityNodeData> nodes, i64& out_root_id, bool& out_repaired)
{
    out_root_id = -1;
    out_repaired = false;

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
        auto parent_id = parent_of.get(node.id).value();
        auto& child_ids = accepted_children.find(node.id)->value;
        if (node.parent_id != parent_id || node.child_ids != child_ids)
            out_repaired = true;
        node.parent_id = parent_id;
        node.child_ids = move(child_ids);
        result.append(move(node));
    }
    if (result.size() != nodes.size())
        out_repaired = true;

    out_root_id = root_id;
    return result;
}

}

void AccessibilityTreeManager::update_tree(Vector<AccessibilityNodeData> nodes)
{
    i64 root_id = -1;
    bool repaired = false;
    auto sanitized = sanitize_tree(move(nodes), root_id, repaired);

    // Detect live-region content changes before replacing the tree. Needs an index of the new nodes by id — so the
    // ancestor walk below is O(depth) rather than O(N) per step. Without this, a full traversal of an N-node tree would
    // be O(N^2) on every tree update. The parent_id links are safe to walk here because sanitize_tree() has already
    // made the tree acyclic and bounded.
    if (on_live_region_changed && !m_nodes.is_empty()) {
        HashMap<i64, AccessibilityNodeData const*> new_nodes_by_id;
        HashTable<i64> announced_regions;
        new_nodes_by_id.ensure_capacity(sanitized.size());
        for (auto const& node : sanitized)
            new_nodes_by_id.set(node.id, &node);

        for (auto const& new_node : sanitized) {
            // Walk up ancestors in the new tree looking for an enclosing live region.
            String live_value;
            i64 live_region_id = -1;
            for (auto const* ancestor = &new_node; ancestor;) {
                if (!ancestor->live.is_empty()) {
                    live_value = ancestor->live;
                    live_region_id = ancestor->id;
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
            if (old_it == m_nodes.end()) {
                // A node that was not in the previous tree. Announce it when the live region enclosing it was
                // already there, because content arriving in an existing region is the case live regions exist
                // for. When the region itself is new, its initial content stays silent - matching how a region
                // added to the document does not announce what it was born holding. One announcement per region
                // per update, since inserting a subtree can present the same text at several new nodes.
                if (!new_node.name.is_empty() && m_nodes.contains(live_region_id) && !announced_regions.contains(live_region_id)) {
                    announced_regions.set(live_region_id);
                    on_live_region_changed(new_node.name, live_value);
                }
                continue;
            }

            auto const& old_node = old_it->value;
            if (old_node.name != new_node.name && !new_node.name.is_empty())
                on_live_region_changed(new_node.name, live_value);
        }
    }

    m_nodes.clear();
    m_root_id = root_id;
    m_whole_tree_was_repaired = repaired;
    m_focus_shown_on_root = false;
    m_focused_id_from_content = {};
    ++m_generation;

    for (auto& node : sanitized) {
        if (node.is_focused)
            m_focused_id_from_content = node.id;
        m_nodes.set(node.id, move(node));
    }
}

Optional<Vector<i64>> AccessibilityTreeManager::apply_delta(AccessibilityTreeDelta const& delta)
{
    // The tree held was checked when it arrived, so only what the delta touches needs checking here: the changed and
    // removed nodes, the nodes whose parent changes, and the subtrees that move. Nothing is changed until all of it
    // passes, so a delta that doesn't fit leaves the tree exactly as it was.
    if (m_root_id == -1)
        return {};

    HashMap<i64, AccessibilityNodeData const*> changed;
    changed.ensure_capacity(delta.changed_nodes.size());
    // Node IDs are never negative, and -1 means "no parent".
    for (auto const& node : delta.changed_nodes) {
        if (node.id < 0)
            return {};
        if (changed.set(node.id, &node) != HashSetResult::InsertedNewEntry)
            return {}; // the same node twice
    }

    HashTable<i64> removed;
    removed.ensure_capacity(delta.removed_ids.size());
    for (auto id : delta.removed_ids) {
        if (id < 0 || id == m_root_id || !m_nodes.contains(id) || changed.contains(id))
            return {};
        if (removed.set(id) != HashSetResult::InsertedNewEntry)
            return {};
    }

    // The node with this ID once the delta is applied, or null if there's none.
    auto final_node = [&](i64 id) -> AccessibilityNodeData const* {
        if (auto it = changed.find(id); it != changed.end())
            return it->value;
        if (removed.contains(id))
            return nullptr;
        return node(id);
    };

    // A node's children are listed by its parent alone, so a node gets a new parent only from a changed node's child
    // list, and no node may appear in two of them (or twice in one).
    HashMap<i64, i64> new_parent;
    for (auto const& node : delta.changed_nodes) {
        for (auto child_id : node.child_ids) {
            if (child_id < 0 || child_id == m_root_id || !final_node(child_id))
                return {};
            if (new_parent.set(child_id, node.id) != HashSetResult::InsertedNewEntry)
                return {};
        }
    }

    auto old_parent_of = [&](i64 id) -> Optional<i64> {
        if (auto it = m_nodes.find(id); it != m_nodes.end())
            return it->value.parent_id;
        return {};
    };
    auto keeps_its_child_list = [&](i64 id) { return !changed.contains(id) && !removed.contains(id); };

    for (auto const& node : delta.changed_nodes) {
        // A node that's new has to be attached somewhere.
        if (!m_nodes.contains(node.id) && !new_parent.contains(node.id))
            return {};
    }
    for (auto const& [child_id, parent_id] : new_parent) {
        // A node that moves has to leave its old parent's child list, so that list has to be one the delta replaces.
        auto old_parent = old_parent_of(child_id);
        if (old_parent.has_value() && *old_parent != parent_id && keeps_its_child_list(*old_parent))
            return {};
    }

    // A child that a replaced child list or a removed node leaves behind has to be removed too, or attached elsewhere;
    // and a removed node can't stay in a child list the delta doesn't replace.
    auto check_left_behind_children = [&](i64 id) {
        for (auto child_id : m_nodes.get(id)->child_ids) {
            if (!removed.contains(child_id) && !new_parent.contains(child_id))
                return false;
        }
        return true;
    };
    for (auto const& node : delta.changed_nodes) {
        if (m_nodes.contains(node.id) && !check_left_behind_children(node.id))
            return {};
    }
    for (auto id : removed) {
        if (!check_left_behind_children(id) || keeps_its_child_list(*old_parent_of(id)))
            return {};
    }

    auto final_parent_of = [&](i64 id) -> i64 {
        if (auto it = new_parent.find(id); it != new_parent.end())
            return it->value;
        return *old_parent_of(id);
    };

    // Every node whose parent changes has to reach the root, through no more than the depth the tree allows. Any cycle
    // runs through such a node, since the tree held had none, so this also rejects every cycle. Then each moved
    // subtree has to fit under that depth too.
    HashMap<i64, size_t> depth_of;
    depth_of.set(m_root_id, 0);
    auto depth_in_final_tree = [&](i64 id) -> Optional<size_t> {
        Vector<i64, 32> chain;
        auto current = id;
        while (!depth_of.contains(current)) {
            if (chain.size() > max_accessibility_tree_depth)
                return {};
            chain.append(current);
            current = final_parent_of(current);
            if (current == -1)
                return {};
        }
        auto depth = depth_of.get(current).value();
        for (size_t i = chain.size(); i > 0; --i) {
            ++depth;
            depth_of.set(chain[i - 1], depth);
        }
        if (depth > max_accessibility_tree_depth)
            return {};
        return depth;
    };

    HashTable<i64> walked;
    for (auto const& [moved_id, parent_id] : new_parent) {
        auto old_parent = old_parent_of(moved_id);
        if (old_parent.has_value() && *old_parent == parent_id)
            continue;
        if (walked.contains(moved_id))
            continue;
        auto depth = depth_in_final_tree(moved_id);
        if (!depth.has_value())
            return {};

        struct Entry {
            i64 id { 0 };
            size_t depth { 0 };
        };
        Vector<Entry> stack;
        stack.append({ moved_id, *depth });
        while (!stack.is_empty()) {
            auto [id, node_depth] = stack.take_last();
            if (walked.set(id) != HashSetResult::InsertedNewEntry)
                continue;
            auto const* node = final_node(id);
            if (!node || (!node->child_ids.is_empty() && node_depth == max_accessibility_tree_depth))
                return {};
            for (auto child_id : node->child_ids)
                stack.append({ child_id, node_depth + 1 });
        }
    }

    // The delta fits. Find what it announces in the tree it produces, while the old data is still at hand to compare
    // against: A changed, nonempty name in a live region that isn't off, or a new node in a live region that was already
    // there, once per region.
    Vector<String> announcement_texts;
    Vector<String> announcement_politeness;
    if (on_live_region_changed) {
        HashTable<i64> announced_regions;
        for (auto const& changed_node : delta.changed_nodes) {
            if (changed_node.name.is_empty())
                continue;
            i64 live_region_id = -1;
            String live_value;
            for (auto id = changed_node.id; id != -1; id = final_parent_of(id)) {
                auto const* ancestor = final_node(id);
                if (!ancestor->live.is_empty()) {
                    live_region_id = id;
                    live_value = ancestor->live;
                    break;
                }
            }
            if (live_value.is_empty() || live_value == "off"sv)
                continue;

            auto old_it = m_nodes.find(changed_node.id);
            if (old_it == m_nodes.end()) {
                if (!m_nodes.contains(live_region_id) || announced_regions.contains(live_region_id))
                    continue;
                announced_regions.set(live_region_id);
            } else if (old_it->value.name == changed_node.name) {
                continue;
            }
            announcement_texts.append(changed_node.name);
            announcement_politeness.append(live_value);
        }
    }

    Vector<i64> removed_ids;
    removed_ids.ensure_capacity(removed.size());
    for (auto id : delta.removed_ids) {
        m_nodes.remove(id);
        removed_ids.append(id);
    }
    for (auto const& [child_id, parent_id] : new_parent) {
        if (!changed.contains(child_id))
            m_nodes.find(child_id)->value.parent_id = parent_id;
    }
    for (auto const& changed_node : delta.changed_nodes) {
        auto parent_id = changed_node.id == m_root_id ? -1 : final_parent_of(changed_node.id);
        auto& stored = m_nodes.ensure(changed_node.id);
        stored = changed_node;
        stored.parent_id = parent_id;
        if (changed_node.is_focused)
            m_focused_id_from_content = changed_node.id;
        else if (m_focused_id_from_content == changed_node.id)
            m_focused_id_from_content = {};
    }
    if (m_focused_id_from_content.has_value() && !m_nodes.contains(*m_focused_id_from_content))
        m_focused_id_from_content = {};
    // A whole tree would have replaced the root's focus that show_focus_on_root() put there, so a change does too.
    if (m_focus_shown_on_root) {
        m_focus_shown_on_root = false;
        for (auto& [id, node] : m_nodes)
            node.is_focused = id == m_focused_id_from_content;
    }
    ++m_generation;

    for (size_t i = 0; i < announcement_texts.size(); ++i)
        on_live_region_changed(move(announcement_texts[i]), move(announcement_politeness[i]));

    return removed_ids;
}

void AccessibilityTreeManager::update_tree(Vector<AccessibilityNodeData> nodes, u64 sequence)
{
    update_tree(move(nodes));
    m_sequence = sequence;
}

Optional<Vector<i64>> AccessibilityTreeManager::apply_delta(AccessibilityTreeDelta const& delta, u64 base_sequence, u64 sequence)
{
    if (m_whole_tree_was_repaired || base_sequence != m_sequence)
        return {};
    auto removed_ids = apply_delta(delta);
    if (removed_ids.has_value())
        m_sequence = sequence;
    return removed_ids;
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
    m_focused_id_from_content = node_id;
    m_focus_shown_on_root = false;
    for (auto& [id, node] : m_nodes)
        node.is_focused = (id == node_id);
}

void AccessibilityTreeManager::show_focus_on_root()
{
    m_focus_shown_on_root = true;
    for (auto& [id, node] : m_nodes)
        node.is_focused = (id == m_root_id);
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
