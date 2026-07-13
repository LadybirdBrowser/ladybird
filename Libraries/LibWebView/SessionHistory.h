/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWebView/Export.h>

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
    using SeedAckProof = u64;

    struct TraversalTarget {
        size_t target_step_index { 0 };
        i32 target_step { 0 };
        Entry const* target_top_level_entry { nullptr };
        bool target_step_is_top_level_entry { false };
        bool changes_top_level_entry { false };
    };

    enum class UpdateResult {
        // WebContent sent the same complete top-level traversable session
        // history that the UI process stores authoritatively.
        CompleteSnapshot,

        // WebContent sent a valid partial view of session history. The UI
        // process merged it into the authoritative history mirror, but
        // WebContent cannot be assumed to know or use every UI history step.
        MergedPartialSnapshot,

        // WebContent sent a snapshot that cannot describe the current UI-owned
        // traversable session history.
        InvalidSnapshot,
    };

    enum class WebContentMutationType {
        CurrentEntryUpdate,
        TopLevelSameDocumentNavigation,
        NestedSameDocumentNavigation,
        NestedCrossDocumentNavigation,
        TopLevelCrossDocumentNavigation,
        AppliedTraversal,
        RestoredCurrentStep,
    };

    struct WebContentMutation {
        WebContentMutationType type { WebContentMutationType::CurrentEntryUpdate };
        Web::HTML::SessionHistoryEntryUpdateKind current_entry_update_kind { Web::HTML::SessionHistoryEntryUpdateKind::NavigationAPIState };
        Web::HTML::CrossProcessId parent_document_state_id;
        Web::HTML::CrossProcessId navigable_id;
        Entry entry;
        Optional<i32> replaced_step;
        i32 current_step { 0 };

        static WebContentMutation current_entry_update(Web::HTML::SessionHistoryEntryUpdateKind update_kind, Entry entry)
        {
            return {
                .type = WebContentMutationType::CurrentEntryUpdate,
                .current_entry_update_kind = update_kind,
                .entry = move(entry),
            };
        }

        static WebContentMutation top_level_same_document_navigation(Entry entry, Optional<i32> replaced_step, i32 current_step)
        {
            return {
                .type = WebContentMutationType::TopLevelSameDocumentNavigation,
                .entry = move(entry),
                .replaced_step = replaced_step,
                .current_step = current_step,
            };
        }

        static WebContentMutation nested_same_document_navigation(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Entry entry, Optional<i32> replaced_step, i32 current_step)
        {
            return {
                .type = WebContentMutationType::NestedSameDocumentNavigation,
                .parent_document_state_id = parent_document_state_id,
                .navigable_id = navigable_id,
                .entry = move(entry),
                .replaced_step = replaced_step,
                .current_step = current_step,
            };
        }

        static WebContentMutation nested_cross_document_navigation(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Entry entry, i32 current_step)
        {
            return {
                .type = WebContentMutationType::NestedCrossDocumentNavigation,
                .parent_document_state_id = parent_document_state_id,
                .navigable_id = navigable_id,
                .entry = move(entry),
                .current_step = current_step,
            };
        }

        static WebContentMutation top_level_cross_document_navigation(Entry entry, i32 current_step)
        {
            return {
                .type = WebContentMutationType::TopLevelCrossDocumentNavigation,
                .entry = move(entry),
                .current_step = current_step,
            };
        }

        static WebContentMutation applied_traversal(i32 step)
        {
            return {
                .type = WebContentMutationType::AppliedTraversal,
                .current_step = step,
            };
        }

        static WebContentMutation restored_current_step(i32 step)
        {
            return {
                .type = WebContentMutationType::RestoredCurrentStep,
                .current_step = step,
            };
        }
    };

    struct WebContentMutationResult {
        bool accepted { false };
        bool web_content_history_matches_mirror { false };
    };

    bool is_empty() const { return m_entries.is_empty(); }
    size_t size() const { return m_entries.size(); }
    size_t used_step_count() const { return m_used_steps.size(); }
    Optional<size_t> current_used_step_index() const { return m_current_used_step_index; }
    Optional<size_t> current_top_level_entry_index() const;

    void clear();
    void navigate(URL::URL, Web::HTML::CrossProcessId document_state_id);
    void navigate(URL::URL, Web::HTML::CrossProcessId document_state_id, Web::HTML::DocumentResource);
    void replace_current_entry_url(URL::URL, Web::HTML::CrossProcessId document_state_id);
    void replace_current_entry(URL::URL, Web::HTML::CrossProcessId document_state_id, Web::HTML::DocumentResource);
    void mark_current_entry_reload_pending();
    void clear_current_entry_reload_pending();
    UpdateResult update_from_web_content(Vector<Entry> entries, Vector<i32> used_steps, size_t current_used_step_index);
    [[nodiscard]] WebContentMutationResult apply_web_content_mutation(WebContentMutation);
    [[nodiscard]] static SeedAckProof compute_seed_ack_proof(Vector<Entry> const&, Vector<i32> const& used_steps, size_t current_used_step_index, Entry const* current_entry_seed_descriptor = nullptr);
    [[nodiscard]] bool web_content_seed_ack_matches_current_mirror(Vector<Entry> const& entries, Vector<i32> const& used_steps, size_t current_used_step_index) const;
    void record_web_content_seeded_from_ui_process(i32 current_step);
    void record_web_content_history_preserved();
    void forget_web_content_state();
    void mark_web_content_history_match_unproven();
    Vector<Entry> entries() const;
    Vector<i32> used_steps() const;
    Vector<Entry> web_content_known_entries() const;
    Vector<i32> web_content_known_used_steps() const;
    Optional<i32> web_content_current_step() const;
    bool web_content_uses_ui_step_coordinates() const { return m_web_content_uses_ui_step_coordinates; }
    bool web_content_history_matches_mirror() const;

    [[nodiscard]] bool can_go_back() const;
    [[nodiscard]] bool can_go_forward() const;
    [[nodiscard]] bool has_only_top_level_used_steps() const;
    [[nodiscard]] bool current_step_is_top_level_entry() const;
    [[nodiscard]] Optional<i32> current_step_to_restore_after_loading_top_level_entry() const;
    [[nodiscard]] bool web_content_can_traverse_to(TraversalTarget const&) const;
    [[nodiscard]] Optional<TraversalTarget> traversal_target_for_delta(int delta) const;
    [[nodiscard]] Optional<TraversalTarget> traversal_target_for_step(i32 step) const;
    [[nodiscard]] Optional<size_t> target_step_index_for_delta(int delta) const;
    [[nodiscard]] Optional<i32> step_at(size_t index) const;
    [[nodiscard]] Entry const* current_entry() const;
    [[nodiscard]] Entry const* entry_at(size_t index) const;
    [[nodiscard]] Entry const* entry_for_step(i32 step) const;
    [[nodiscard]] Entry const* top_level_entry_for_step(i32 step) const;

    void traverse_to(size_t index);

private:
    [[nodiscard]] bool web_content_known_history_matches_mirror() const;
    [[nodiscard]] bool update_current_entry_from_web_content(Web::HTML::SessionHistoryEntryUpdateKind, Entry);
    [[nodiscard]] bool apply_top_level_same_document_navigation_from_web_content(Entry, Optional<i32> replaced_step, i32 current_step);
    [[nodiscard]] bool apply_nested_same_document_navigation_from_web_content(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Entry, Optional<i32> replaced_step, i32 current_step);
    [[nodiscard]] bool apply_nested_cross_document_navigation_from_web_content(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Entry, i32 current_step);
    [[nodiscard]] bool apply_top_level_cross_document_navigation_from_web_content(Entry, i32 current_step);
    [[nodiscard]] bool did_restore_web_content_to_current_step(i32 step);
    [[nodiscard]] bool did_apply_web_content_traversal_to_step(i32 step);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
    Vector<Entry> m_entries;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
    Vector<i32> m_used_steps;

    // Index of the current session history step within m_used_steps.
    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    Optional<size_t> m_current_used_step_index;

    // WebContent's latest current session history step, translated into the
    // UI-owned traversable session history's step coordinate space.
    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    Vector<Entry> m_web_content_known_entries;
    Vector<i32> m_web_content_known_used_steps;
    Optional<i32> m_web_content_current_step;
    // False when a partial snapshot was translated into the UI-owned step
    // coordinate space. In that state WebContent still uses its original step
    // numbers, so the UI must reseed/load instead of delegating traversal by step.
    bool m_web_content_uses_ui_step_coordinates { false };

    bool m_web_content_history_match_is_proven { false };
};

}
