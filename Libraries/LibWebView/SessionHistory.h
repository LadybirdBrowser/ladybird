/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebCommon/HTML/ApplyHistoryStep.h>
#include <LibWebCommon/HTML/HistoryHandlingBehavior.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/HTML/UserNavigationInvolvement.h>
#include <LibWebView/CanonicalSessionHistoryEntry.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

inline constexpr size_t MAX_NESTED_HISTORY_DEPTH = 16;

// NB: The UI process holds the traversable's session history, so that it survives WebContent process swaps and crash
//     recovery. Entries cross to WebContent and to session storage as descriptors.
//
// https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
class WEBVIEW_API TraversableSessionHistory {
public:
    struct TraversalTarget {
        size_t target_step_index { 0 };
        bool changes_top_level_entry { false };
    };

    explicit TraversableSessionHistory(NonnullRefPtr<CanonicalSessionHistoryEntry> initial_history_entry);
    TraversableSessionHistory(TraversableSessionHistory&&) = default;
    TraversableSessionHistory& operator=(TraversableSessionHistory&&) = default;

    size_t size() const { return m_entries.size(); }
    size_t used_step_count() const { return used_steps().size(); }
    i32 current_step() const { return m_current_session_history_step; }
    // The index in the used steps of the used step the current step resolves to.
    size_t current_used_step_index() const;
    size_t current_top_level_entry_index() const;

    bool initialize_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor>, Vector<i32> used_steps, size_t current_used_step_index);
    [[nodiscard]] ErrorOr<void> restore_from_ui_snapshot(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, Function<Web::HTML::CrossProcessId()> allocate_cross_process_id);
    Optional<i32> append_nested_history(CanonicalNavigable const& parent_navigable, CanonicalDocumentState& parent_document_state, Web::HTML::CrossProcessId child_navigable_id, NonnullRefPtr<CanonicalSessionHistoryEntry> history_entry);
    bool remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id);
    void clear_the_forward_session_history();
    // Clears the forward session history, then appends entry to navigable's session history entries at the step after
    // the current one, which it returns.
    i32 push_session_history_entry(CanonicalNavigable const&, NonnullRefPtr<CanonicalSessionHistoryEntry>);
    // Replaces entry_to_replace in navigable's session history entries with entry, at its step. Changes nothing if
    // entry_to_replace is not among them.
    bool replace_session_history_entry(CanonicalNavigable const&, CanonicalSessionHistoryEntry const& entry_to_replace, NonnullRefPtr<CanonicalSessionHistoryEntry>);
    void update_directive_state_value(Web::HTML::CrossProcessId, Optional<String>);
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries() const;
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
    Vector<i32> used_steps() const;

    [[nodiscard]] bool has_only_top_level_used_steps() const;
    [[nodiscard]] Optional<TraversalTarget> traversal_target_for_step(i32 step) const;
    [[nodiscard]] Optional<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const&> get_session_history_entries(CanonicalNavigable const&) const;
    [[nodiscard]] Optional<i32> get_the_used_step(i32 step) const;
    [[nodiscard]] CanonicalSessionHistoryEntry* get_the_target_history_entry(CanonicalNavigable const&, i32 step) const;
    [[nodiscard]] Web::HTML::HistoryObjectLengthAndIndex get_the_history_object_length_and_index(i32 step) const;
    [[nodiscard]] Optional<Vector<Web::HTML::SessionHistoryEntryDescriptor>> get_session_history_entries_for_the_navigation_api(CanonicalNavigable const&, i32 target_step) const;
    [[nodiscard]] Vector<CanonicalNavigable&> get_all_navigables_whose_current_session_history_entry_will_change_or_reload(CanonicalNavigable& traversable, i32 target_step) const;
    [[nodiscard]] Vector<Web::HTML::CrossProcessId> get_all_navigables_that_might_experience_a_cross_document_traversal(CanonicalNavigable const& traversable, i32 target_step) const;
    [[nodiscard]] Vector<Web::HTML::CrossProcessId> get_all_navigables_that_only_need_history_object_length_index_update(CanonicalNavigable const& traversable, i32 target_step) const;
    void set_current_session_history_step(i32 step)
    {
        VERIFY(used_steps().contains_slow(step));
        m_current_session_history_step = step;
    }
    [[nodiscard]] CanonicalSessionHistoryEntry& current_entry() const;
    [[nodiscard]] CanonicalSessionHistoryEntry* entry_at(size_t index) const;
    [[nodiscard]] CanonicalSessionHistoryEntry* top_level_entry_for_step(i32 step) const;

private:
    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
    Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> m_entries;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    // NB: A step that removing a navigable or replacing an entry made unused stays until applying the history step
    //     resolves it to the used step at or before it, as getting the used step does.
    i32 m_current_session_history_step { 0 };
};

struct SessionHistorySnapshot {
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries;
    Vector<i32> used_steps;
    size_t current_used_step_index { 0 };
};

WEBVIEW_API ErrorOr<void> validate_snapshot_is_restorable(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Vector<i32> const& used_steps, size_t current_used_step_index);
WEBVIEW_API URL::URL display_url_for_session_history_entry(Web::HTML::SessionHistoryEntryDescriptor const&, bool include_fragment_directive = true);

}
