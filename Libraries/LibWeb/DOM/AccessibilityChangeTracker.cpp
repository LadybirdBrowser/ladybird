/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AccessibilityChangeTracker.h>

namespace Web::DOM {

void AccessibilityChangeTracker::begin(i64 document_id, bool whole_tree)
{
    m_whole_tree = whole_tree || needs_whole_tree(document_id);
    if (m_whole_tree)
        m_states.clear();
    m_document_id = document_id;
    ++m_build;
    m_known_at_begin = m_states.size();
    m_known_noted = 0;
}

// A hash of the fields that a build computes again for every node, kept or not.
static u64 hash_computed_fields(WebView::AccessibilityNodeData const& data)
{
    u64 hash = 0;
    auto mix = [&](u64 value) {
        hash ^= value;
        hash *= 0x9e3779b97f4a7c15;
        hash ^= hash >> 29;
    };
    mix(static_cast<u64>(data.parent_id));
    mix(data.child_ids.size());
    for (auto id : data.child_ids)
        mix(static_cast<u64>(id));
    mix((static_cast<u64>(static_cast<u32>(data.bounds.x())) << 32) | static_cast<u32>(data.bounds.y()));
    mix((static_cast<u64>(static_cast<u32>(data.bounds.width())) << 32) | static_cast<u32>(data.bounds.height()));
    mix((static_cast<u64>(static_cast<u32>(data.caret_offset)) << 32) | static_cast<u32>(data.selection_start));
    mix((static_cast<u64>(static_cast<u32>(data.selection_end)) << 32) | (data.is_focused ? 1 : 0));
    return hash;
}

bool AccessibilityChangeTracker::note(WebView::AccessibilityNodeData const& data, u64 record_version, Optional<u64> geometry_version)
{
    auto known_geometry_version = geometry_version.value_or(UNKNOWN_GEOMETRY_VERSION);
    auto computed_fields_hash = hash_computed_fields(data);
    auto it = m_states.find(data.id);
    if (it == m_states.end()) {
        m_states.set(data.id, {});
        it = m_states.find(data.id);
    } else if (it->value.build != m_build) {
        ++m_known_noted;
        auto const& state = it->value;
        auto unchanged = !m_whole_tree && record_version != 0 && state.record_version == record_version
            && known_geometry_version != UNKNOWN_GEOMETRY_VERSION && state.geometry_version == known_geometry_version
            && state.computed_fields_hash == computed_fields_hash;
        if (unchanged) {
            it->value.build = m_build;
            return false;
        }
    }
    it->value = { record_version, known_geometry_version, computed_fields_hash, m_build };
    return true;
}

Vector<i64> AccessibilityChangeTracker::end()
{
    // Only when fewer of the nodes noted last time were noted this time are any of them gone.
    Vector<i64> removed_ids;
    if (m_known_noted == m_known_at_begin)
        return removed_ids;
    for (auto const& [id, state] : m_states) {
        if (state.build != m_build)
            removed_ids.append(id);
    }
    for (auto id : removed_ids)
        m_states.remove(id);
    return removed_ids;
}

}
