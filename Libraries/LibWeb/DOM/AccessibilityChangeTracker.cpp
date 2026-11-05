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

bool AccessibilityChangeTracker::note(WebView::AccessibilityNodeData const& data, u64 record_version, Optional<u64> geometry_version)
{
    auto it = m_states.find(data.id);
    if (it == m_states.end()) {
        m_states.set(data.id, {});
        it = m_states.find(data.id);
    } else if (it->value.build != m_build) {
        ++m_known_noted;
        auto const& state = it->value;
        auto unchanged = !m_whole_tree && record_version != 0 && state.record_version == record_version
            && geometry_version.has_value() && state.geometry_version == geometry_version
            && state.parent_id == data.parent_id && state.child_ids == data.child_ids && state.bounds == data.bounds
            && state.is_focused == data.is_focused && state.caret_offset == data.caret_offset
            && state.selection_start == data.selection_start && state.selection_end == data.selection_end;
        if (unchanged) {
            it->value.build = m_build;
            return false;
        }
    }
    auto& state = it->value;
    state.record_version = record_version;
    state.geometry_version = geometry_version;
    state.parent_id = data.parent_id;
    state.child_ids = data.child_ids;
    state.bounds = data.bounds;
    state.is_focused = data.is_focused;
    state.caret_offset = data.caret_offset;
    state.selection_start = data.selection_start;
    state.selection_end = data.selection_end;
    state.build = m_build;
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
