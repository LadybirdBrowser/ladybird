/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// AD-HOC: The HTML Standard stores a traversable navigable's session history entries on the traversable. Ladybird
//         keeps an IPC-serializable mirror in the UI process so browser history survives WebContent process swaps
//         and crash recovery. The mirror still uses the spec's session history entry and all used history steps model.
//
// https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
class WEBVIEW_API TraversableSessionHistory {
public:
    using Entry = Web::HTML::SessionHistoryEntryDescriptor;

    struct TraversalTarget {
        size_t target_step_index { 0 };
        i32 target_step { 0 };
        size_t target_top_level_entry_index { 0 };
        Entry const* target_top_level_entry { nullptr };
        bool target_step_is_top_level_entry { false };
        bool changes_top_level_entry { false };
    };

    bool is_empty() const { return m_entries.is_empty(); }
    size_t size() const { return m_entries.size(); }
    size_t used_step_count() const { return m_used_steps.size(); }
    Optional<size_t> current_used_step_index() const { return m_current_used_step_index; }
    Optional<i32> current_step() const
    {
        if (!m_current_used_step_index.has_value() || *m_current_used_step_index >= m_used_steps.size())
            return {};
        return m_used_steps[*m_current_used_step_index];
    }
    Optional<size_t> current_top_level_entry_index() const;

    void clear();
    bool initialize_for_testing(Vector<Entry>, Vector<i32> used_steps, size_t current_used_step_index);
    void initialize_with_initial_history_entry(Entry initial_history_entry);
    void mark_current_entry_reload_pending();
    void clear_current_entry_reload_pending();
    bool update_entry(Optional<Web::HTML::CrossProcessId> nested_history_id, Utf16String const& navigation_api_key, Function<void(Entry&)> const& update_entry);
    bool update_entry_persisted_state(Optional<Web::HTML::CrossProcessId> nested_history_id, Web::HTML::SessionHistoryEntryPersistedState const&);
    bool update_document_state(Optional<Web::HTML::CrossProcessId> nested_history_id, Utf16String const& navigation_api_key, Function<void(Web::HTML::SessionHistoryDocumentStateDescriptor&)> const& update_document_state);
    Optional<i32> append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor);
    bool remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id);
    Optional<i32> finalize_same_document_navigation(CanonicalNavigable const&, Web::HTML::SameDocumentNavigationEntry target_entry, Optional<Utf16String> entry_to_replace_navigation_api_key);
    Optional<i32> finalize_cross_document_navigation(Optional<Web::HTML::CrossProcessId> nested_history_id, Web::HTML::PendingSessionHistoryEntryDescriptor history_entry, Optional<Utf16String> entry_to_replace_navigation_api_key);
    Vector<Entry> entries() const;
    Vector<i32> used_steps() const;

    [[nodiscard]] bool can_go_back() const;
    [[nodiscard]] bool can_go_forward() const;
    [[nodiscard]] bool has_only_top_level_used_steps() const;
    [[nodiscard]] Optional<TraversalTarget> traversal_target_for_delta(int delta) const;
    [[nodiscard]] Optional<TraversalTarget> traversal_target_for_step(i32 step) const;
    [[nodiscard]] Optional<Vector<Entry> const&> get_session_history_entries(CanonicalNavigable const&) const;
    [[nodiscard]] Optional<i32> get_the_used_step(i32 step) const;
    [[nodiscard]] Entry const* get_the_target_history_entry(CanonicalNavigable const&, i32 step) const;
    [[nodiscard]] Optional<Web::HTML::HistoryObjectLengthAndIndex> get_the_history_object_length_and_index(i32 step) const;
    [[nodiscard]] Optional<Vector<Entry>> get_session_history_entries_for_the_navigation_api(CanonicalNavigable const&, i32 target_step) const;
    [[nodiscard]] Vector<Web::HTML::CrossProcessId> get_all_navigables_whose_current_session_history_entry_will_change_or_reload(CanonicalNavigable const& traversable, i32 target_step) const;
    [[nodiscard]] Vector<Web::HTML::CrossProcessId> get_all_navigables_that_might_experience_a_cross_document_traversal(CanonicalNavigable const& traversable, i32 target_step) const;
    [[nodiscard]] Vector<Web::HTML::CrossProcessId> get_all_navigables_that_only_need_history_object_length_index_update(CanonicalNavigable const& traversable, i32 target_step) const;
    void set_current_session_history_step(i32 step)
    {
        auto index = m_used_steps.find_first_index(step);
        VERIFY(index.has_value());
        m_current_used_step_index = *index;
    }
    [[nodiscard]] Optional<size_t> target_step_index_for_delta(int delta) const;
    [[nodiscard]] Optional<i32> step_at(size_t index) const;
    [[nodiscard]] Entry const* current_entry() const;
    [[nodiscard]] Entry const* entry_at(size_t index) const;
    [[nodiscard]] Entry const* entry_for_step(i32 step) const;
    [[nodiscard]] Entry const* top_level_entry_for_step(i32 step) const;

    void traverse_to(size_t index);

private:
    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
    Vector<Entry> m_entries;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
    Vector<i32> m_used_steps;

    // Index of the current session history step within m_used_steps.
    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    Optional<size_t> m_current_used_step_index;
};

}
