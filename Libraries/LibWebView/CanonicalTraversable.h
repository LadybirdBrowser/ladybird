/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/Page.h>
#include <LibWebView/ApplyHistoryStep.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Export.h>
#include <LibWebView/SessionHistory.h>
#include <LibWebView/SessionHistoryTraversalQueue.h>

namespace WebView {

// NB: The HTML Standard spells this algorithm argument "checkForCancelation".
enum class CheckForCancelation : u8 {
    Yes,
    No,
};

class WEBVIEW_API CanonicalTraversable final
    : public CanonicalNavigable {
public:
    CanonicalTraversable();
    virtual ~CanonicalTraversable() override;

    virtual bool is_top_level_traversable() const override { return true; }

    // Apply-the-history-step coordination. Operations serialize on the traversable's session history traversal
    // queue; the algorithm runs here and dispatches its per-navigable jobs to the processes hosting the documents.
    using OnHistoryOperationComplete = Function<void(Web::HTML::HistoryStepResult, Optional<i32> committed_step)>;

    struct BrowserHistoryTraversalDiagnostic {
        enum class Stage : u8 {
            ApplyingInWebContent,
            CheckingCancelation,
        };

        i32 target_step { 0 };
        size_t target_step_index { 0 };
        bool changes_top_level_entry { false };
        Stage stage { Stage::ApplyingInWebContent };
    };

    void enqueue_history_operation(u64 initiation_id, Web::HistoryOperationParameters, WebContentClient& requesting_client, u64 requesting_page_id, OnHistoryOperationComplete = nullptr);
    // Appends plain algorithm steps; a requested traversal defers its target resolution to its queued position, the
    // way the specification's queued steps do, and then starts its operation at that position.
    void append_history_queue_steps(SessionHistoryTraversalSteps);
    void run_history_operation_at_queue_position(u64 initiation_id, Web::HistoryOperationParameters, WebContentClient& requesting_client, u64 requesting_page_id, Optional<i32> resolved_step, OnHistoryOperationComplete, NonnullRefPtr<Core::Promise<Empty>>);
    void abandon_history_operations();

    struct HistoryJobEndpoint {
        RefPtr<WebContentClient> client;
        u64 page_id { 0 };
    };
    HistoryJobEndpoint history_job_endpoint_for(CanonicalNavigable const&) const;

    void did_receive_history_operation_ready(u64 operation_id, Web::HistoryOperationReadyResult);
    void did_receive_history_step_unload_cancelation_result(u64 operation_id, Web::HTML::HistoryStepResult);
    void did_receive_changing_navigable_history_job_ready(WebContentClient&, u64 source_page_id, u64 operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition);
    void did_receive_changing_navigable_continuation_applied(WebContentClient&, u64 source_page_id, u64 operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state);
    void did_receive_nonchanging_navigable_history_state_updated(WebContentClient&, u64 source_page_id, u64 operation_id, Web::HTML::CrossProcessId navigable_id);

    CanonicalNavigable& insert(WebContentClient& reporting_client, u64 page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState, CanonicalNavigable& fallback_parent);
    Optional<CanonicalNavigable&> find(Web::HTML::CrossProcessId navigable_id);
    Optional<CanonicalNavigable const&> find(Web::HTML::CrossProcessId navigable_id) const;
    void remove(CanonicalNavigable&);

    TraversableSessionHistory const& session_history() const { return m_session_history; }
    Optional<size_t> effective_current_session_history_step_index() const;

    StorageJar& session_storage() { return *m_session_storage; }
    void clone_session_storage_from(CanonicalTraversable const&);

    // Fired after every canonical session-history mutation, once the history has reached its post-mutation state.
    Function<void()> on_session_history_changed;

    Web::HTML::VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(Web::HTML::VisibilityState visibility_state) { m_system_visibility_state = visibility_state; }

    Optional<BrowserHistoryTraversalDiagnostic> browser_history_traversal_for_testing() const;
    Web::HTML::SessionHistoryEntryDescriptor const* ongoing_browser_history_traversal_target_entry() const;

    void prepare_for_reload();
    void did_create_top_level_traversable(Web::HTML::SessionHistoryEntryDescriptor initial_history_entry);
    bool update_session_history_entry_navigation_api_state(CanonicalNavigable const&, Utf16String const& navigation_api_key, Web::HTML::StorageSerializationRecord navigation_api_state);
    bool update_session_history_entry_scroll_restoration_mode(CanonicalNavigable const&, Utf16String const& navigation_api_key, Web::HTML::ScrollRestorationMode scroll_restoration_mode);
    bool update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable const&, Utf16String const& navigation_api_key, Utf16String navigable_target_name);
    bool set_session_history_entry_document_state_reload_pending(CanonicalNavigable const&, Utf16String const& navigation_api_key, bool reload_pending);
    Optional<i32> append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry);
    bool remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id);
    Optional<i32> navigation_api_traversal_target(CanonicalNavigable const&, Utf16String const& navigation_api_key) const;
    void traverse_the_history_by_delta(int delta, CheckForCancelation, Function<void()> on_ready = nullptr);
    void traverse_the_history_to_step(i32 step, CheckForCancelation, Function<void()> on_ready = nullptr);
    void reconstruct_the_history_to_step(i32 step);
    ErrorOr<URL::URL> restore_session_history_from_ui_snapshot(SessionHistorySnapshot);
    void abandon_after_web_content_process_crash();
    void recover_from_web_content_process_crash(Optional<HistoryJobEndpoint> crashed_endpoint, OnHistoryOperationComplete);
    void reset_session_history_for_testing(Web::HTML::SessionHistoryEntryDescriptor);

    static StringView browser_history_traversal_stage_to_string(BrowserHistoryTraversalDiagnostic::Stage);

private:
    struct HistoryOperation;
    void session_history_changed();
    HistoryOperation* find_history_operation(u64 operation_id);
    void add_history_operation_completion_endpoint(HistoryOperation&, HistoryJobEndpoint, Optional<u64> initiation_id = {});
    bool select_changing_navigable_history_step_job_endpoint(HistoryOperation&, ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob const&);
    void dispatch_changing_navigable_history_step_job(HistoryOperation&, Web::HTML::CrossProcessId navigable_id);
    void dispatch_changing_navigable_history_step_continuation(HistoryOperation&, Web::HTML::CrossProcessId navigable_id);
    void dispatch_crash_recovery_changing_job(HistoryOperation&, HistoryJobEndpoint, Web::HTML::HistoryObjectLengthAndIndex, Function<void()> on_complete);
    void complete_history_jobs_after_crash(HistoryOperation&, Vector<Web::HTML::CrossProcessId> changing_jobs, Vector<Web::HTML::CrossProcessId> nonchanging_updates);
    void finish_deferred_history_operation_after_crash_recovery(u64 operation_id);
    ApplyHistoryStepJobs create_apply_history_step_jobs(u64 operation_id);
    void enqueue_browser_history_traversal(Web::TraverseToStepHistoryOperationParameters, bool check_for_cancelation, OnHistoryOperationComplete = nullptr);
    void run_browser_history_traversal_at_queue_position(Web::TraverseToStepHistoryOperationParameters, bool check_for_cancelation, Function<void()> on_ready, OnHistoryOperationComplete, NonnullRefPtr<Core::Promise<Empty>>);
    void start_history_operation(HistoryOperation&, NonnullRefPtr<Core::Promise<Empty>>);
    void apply_history_step(HistoryOperation&, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement, Optional<Web::Bindings::NavigationType>, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot = {});
    void update_for_navigable_creation_or_destruction(HistoryOperation&);
    void finish_history_operation(u64 operation_id, Web::HTML::HistoryStepResult, Optional<i32> committed_step);
    HistoryOperation* ongoing_browser_history_traversal();

    struct PendingBrowserHistoryTraversal {
        enum class Stage : u8 {
            Queued,
            Running,
        };

        u64 generation { 0 };
        Vector<int> deltas;
        Optional<i32> target_step;
        Optional<u64> operation_id;
        Vector<Function<void()>> on_ready_callbacks;
        CheckForCancelation check_for_cancelation { CheckForCancelation::Yes };
        Stage stage { Stage::Queued };
    };
    Optional<TraversableSessionHistory::TraversalTarget> browser_traversal_target_for_delta(Optional<i32> base_step, int delta) const;
    Optional<TraversableSessionHistory::TraversalTarget> pending_browser_history_traversal_target() const;
    void queue_browser_history_traversal(Optional<i32> target_step, Optional<int> delta, CheckForCancelation, Function<void()> on_ready);
    void start_pending_browser_history_traversal(u64 generation, NonnullRefPtr<Core::Promise<Empty>>);
    void supersede_browser_history_traversal_by_delta(HistoryOperation&, int delta, Function<void()> on_ready);
    void supersede_browser_history_traversal(HistoryOperation&, TraversableSessionHistory::TraversalTarget, Function<void()> on_ready);
    void run_pending_browser_history_traversal(TraversableSessionHistory::TraversalTarget, NonnullRefPtr<Core::Promise<Empty>>);
    Function<void()> take_pending_browser_history_traversal_on_ready();

    Optional<Web::HTML::CrossProcessId> nested_history_id_for(CanonicalNavigable const&) const;
    void traverse_the_history(TraversableSessionHistory::TraversalTarget const&, CheckForCancelation, Function<void()> on_ready, NonnullRefPtr<Core::Promise<Empty>>);
    void remove_from_index(CanonicalNavigable&);

    HashMap<Web::HTML::CrossProcessId, WeakPtr<CanonicalNavigable>> m_navigable_index;
    TraversableSessionHistory m_session_history;

    // https://storage.spec.whatwg.org/#storage-sheds
    // A traversable navigable's storage shed holds all session storage data.
    NonnullOwnPtr<StorageJar> m_session_storage;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue
    SessionHistoryTraversalQueue m_history_traversal_queue;
    TraversableApplyHistoryStepState m_apply_history_step_traversable_state;
    u64 m_next_history_operation_id { 1 };
    u64 m_next_pending_browser_history_traversal_generation { 1 };
    HashMap<u64, NonnullOwnPtr<HistoryOperation>> m_history_operations;
    Optional<PendingBrowserHistoryTraversal> m_pending_browser_history_traversal;
    Web::HTML::VisibilityState m_system_visibility_state { Web::HTML::VisibilityState::Hidden };
};

}
