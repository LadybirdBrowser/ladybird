/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AnyOf.h>
#include <AK/ByteString.h>
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
#include <LibCompositing/PageId.h>
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
#include <LibWebView/WebContentPage.h>

namespace WebView {

// NB: The HTML Standard spells this algorithm argument "checkForCancelation".
enum class CheckForCancelation : u8 {
    Yes,
    No,
};

enum class WebContentProcessLost : bool {
    No,
    Yes,
};

class WEBVIEW_API CanonicalTraversable final
    : public CanonicalNavigable {
public:
    explicit CanonicalTraversable(Web::HTML::CrossProcessId id = {});
    virtual ~CanonicalTraversable() override;

    virtual bool is_top_level_traversable() const override { return true; }
    virtual void clear_ongoing_navigation() override;
    CanonicalDocument& document_active_in(CanonicalNavigable&, WebContentPage const&) const;

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

    void enqueue_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters, RefPtr<WebContentPage> requesting_page, u64 sequence_number, OnHistoryOperationComplete = nullptr);
    // Appends plain algorithm steps; a requested traversal defers its target resolution to its queued position, the
    // way the specification's queued steps do, and then starts its operation at that position.
    void append_history_queue_steps(SessionHistoryTraversalSteps);
    void run_history_operation_at_queue_position(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters, RefPtr<WebContentPage> requesting_page, u64 sequence_number, OnHistoryOperationComplete, NonnullRefPtr<Core::Promise<Empty>>);
    u64 next_sequence_number() { return m_next_sequence_number++; }
    void abandon_history_operations();

    RefPtr<WebContentPage> page_hosting(CanonicalNavigable const&) const;

    void did_receive_history_operation_ready(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationReadyResult);
    void did_receive_history_step_unload_cancelation_result(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult, Web::HTML::UnloadPromptShown);
    void did_receive_beforeunload_check_result(WebContentPage& source_page, Web::HTML::CrossProcessId check_id, Web::HTML::HistoryStepResult, Web::HTML::UnloadPromptShown);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
    void check_if_unloading_is_canceled(Vector<Web::HTML::CrossProcessId> navigable_ids, RefPtr<WebContentPage> skipped_endpoint, Web::HTML::UnloadPromptShown, Function<void(Web::HTML::HistoryStepResult, Web::HTML::UnloadPromptShown)> on_complete);
    void did_receive_changing_navigable_history_job_ready(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition, Web::HTML::UnloadDisplayedDocument);
    void did_finish_history_navigation_params_creation(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation);
    void did_receive_changing_navigable_unload_preparation_complete(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id);
    void did_receive_descendant_unload_task_complete(WebContentPage& source_page, Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id);
    void did_receive_child_navigable_unload_request(WebContentPage& source_page, Web::HTML::CrossProcessId navigable_id);
    void did_receive_changing_navigable_continuation_applied(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state);
    void did_receive_nonchanging_navigable_history_state_updated(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id);

    CanonicalNavigable& insert(NonnullRefPtr<WebContentPage> reporting_page, CanonicalNavigable& parent, CanonicalDocument& container_document, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState, NonnullRefPtr<CanonicalDocument>);
    Optional<CanonicalNavigable&> find(Web::HTML::CrossProcessId navigable_id);
    Optional<CanonicalNavigable const&> find(Web::HTML::CrossProcessId navigable_id) const;
    void remove(CanonicalNavigable&);

    Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigable_graph() const;

    void for_each_hosting_page(Function<void(WebContentPage&)> const&) const;
    void for_each_opener_traversable(Function<void(CanonicalTraversable&)> const&) const;
    void represent_openers_in(WebContentClient&);
    bool is_opener_page(WebContentPage const& page) const
    {
        return any_of(m_opener_pages, [&](auto const& opener_page) { return opener_page.ptr() == &page; });
    }
    void forget_opener_page(WebContentPage&);
    void discard_opener_pages();
    void for_each_page_representing(CanonicalNavigable const&, Function<void(WebContentPage&)> const&) const;
    bool hosts(CanonicalNavigable const&, WebContentPage const&) const;
    bool represents(CanonicalNavigable const&, WebContentPage const&) const;
    bool page_hosts_any(WebContentPage const&) const;
    void stop_hosting_in_page(CanonicalNavigable&, NonnullRefPtr<WebContentPage>);
    void release_page_if_unused(NonnullRefPtr<WebContentPage>);

    Optional<ViewImplementation&> view() const;
    void set_view(Badge<ViewImplementation>, ViewImplementation&);
    RefPtr<WebContentPage> display_page() const;
    bool display_page_is_pending() const;
    RefPtr<WebContentPage> displaced_document_host() const;
    bool is_displaced_document_host(WebContentPage const& page) const { return displaced_document_host().ptr() == &page; }
    void release_displaced_document_host();
    void release_displaced_document_host_after_unload();
    void discard_displaced_document_host();
    void did_lose_page(WebContentPage&, WebContentProcessLost);

    TraversableSessionHistory const& session_history() const { return m_session_history; }
    Optional<size_t> effective_current_session_history_step_index() const;

    StorageJar& session_storage() { return *m_session_storage; }
    void clone_session_storage_from(CanonicalTraversable const&);

    // Fired after every canonical session-history mutation, once the history has reached its post-mutation state.
    Function<void()> on_session_history_changed;

    Web::HTML::VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(Web::HTML::VisibilityState);

    bool has_system_focus() const { return m_has_system_focus; }
    void set_has_system_focus(bool, RefPtr<WebContentPage> requesting_page);

    Optional<Web::HTML::CrossProcessId> const& focused_navigable_id() const { return m_focused_navigable_id; }
    void set_focused_navigable(CanonicalNavigable&, WebContentPage& requesting_page);
    RefPtr<WebContentPage> focused_navigable_host() const;
    Compositing::DevicePixelPoint focused_navigable_host_offset() const;
    Compositing::DevicePixelPoint local_root_offset(CanonicalNavigable const&) const;

    Optional<BrowserHistoryTraversalDiagnostic> browser_history_traversal_for_testing() const;
    Web::HTML::SessionHistoryEntryDescriptor const* ongoing_browser_history_traversal_target_entry() const;
    ByteString pending_same_document_session_history_entries_for_debug() const;

    void prepare_for_reload();
    static CanonicalTraversable& create_a_new_top_level_traversable(Web::HTML::CrossProcessId id, Optional<CanonicalNavigable&> opener, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, WebContentClient& process);
    static void remove_from_user_agent_top_level_traversable_set(CanonicalTraversable&);
    bool update_session_history_entry_navigation_api_state(CanonicalNavigable&, Web::HTML::SessionHistoryEntryIdentity const&, Web::HTML::StorageSerializationRecord navigation_api_state);
    bool update_session_history_entry_scroll_restoration_mode(CanonicalNavigable&, Web::HTML::SessionHistoryEntryIdentity const&, Web::HTML::ScrollRestorationMode scroll_restoration_mode);
    bool update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable&, Web::HTML::SessionHistoryEntryIdentity const&, Utf16String navigable_target_name);
    bool set_session_history_entry_document_state_reload_pending(CanonicalNavigable const&, Utf16String const& navigation_api_key, bool reload_pending);
    Optional<i32> append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry);
    bool remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id);
    void traverse_the_history_by_delta(int delta, CheckForCancelation, Function<void()> on_ready = nullptr);
    void traverse_the_history_to_step(i32 step, CheckForCancelation, Function<void()> on_ready = nullptr);
    void reconstruct_the_history_to_step(i32 step);
    ErrorOr<URL::URL> restore_session_history_from_ui_snapshot(SessionHistorySnapshot);
    void abandon_after_web_content_process_crash();
    void recover_from_web_content_process_crash(RefPtr<WebContentPage> crashed_endpoint, OnHistoryOperationComplete);
    void reset_session_history_for_testing(Web::HTML::SessionHistoryEntryDescriptor);
    bool initialize_session_history_for_testing(Vector<TraversableSessionHistory::Entry>, Vector<i32> used_steps, size_t current_used_step_index);

    static StringView browser_history_traversal_stage_to_string(BrowserHistoryTraversalDiagnostic::Stage);

private:
    ViewImplementation* m_view { nullptr };
    struct HistoryOperation;
    void session_history_changed();
    HistoryOperation* find_history_operation(Web::HTML::CrossProcessId operation_id);
    bool navigation_transaction_matches(HistoryOperation const&, WebContentPage&, Optional<Web::HTML::CrossProcessId> reply_navigable_id = {}) const;
    bool update_session_history_entry_persisted_state(CanonicalNavigable&, Web::HTML::SessionHistoryEntryPersistedState const&);
    bool discard_pending_same_document_session_history_entries_for_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters const&);
    void add_history_operation_completion_endpoint(HistoryOperation&, NonnullRefPtr<WebContentPage>);
    bool select_changing_navigable_history_step_job_endpoint(HistoryOperation&, ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob&);
    void dispatch_changing_navigable_history_step_job(HistoryOperation&, Web::HTML::CrossProcessId navigable_id);
    void continue_history_navigation_population(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id);
    void dispatch_changing_navigable_history_step_continuation(HistoryOperation&, Web::HTML::CrossProcessId navigable_id);
    void send_changing_navigable_continuation_task(HistoryOperation&, Web::HTML::CrossProcessId navigable_id, Web::HTML::UnloadDisplayedDocument);
    void deactivate_a_document_for_cross_document_navigation(HistoryOperation&, Web::HTML::CrossProcessId navigable_id);
    void unload_displayed_document_for_cross_document_navigation(HistoryOperation&, Web::HTML::CrossProcessId navigable_id);
    void did_activate_history_entry(HistoryOperation&, Web::HTML::CrossProcessId navigable_id, NonnullRefPtr<WebContentPage> source_page, Web::HTML::SessionHistoryEntryDescriptor const& target_entry, CanonicalNavigable::DidPopulateDocument, Web::HTML::ReplicatedNavigableState activated_navigable_state);
    enum class UnloadedInItsHost : bool {
        No,
        Yes,
    };
    void unload_a_document_and_its_descendants(Optional<Web::HTML::CrossProcessId> operation_id, Web::HTML::CrossProcessId navigable_id, RefPtr<WebContentPage> continuing_endpoint, Web::HTML::ChildNavigableDestruction, Function<void(UnloadedInItsHost)> queue_document_unload_task);
    void unload_document_in_its_host(Optional<Web::HTML::CrossProcessId> operation_id, NonnullRefPtr<WebContentPage>, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChildNavigableDestruction, Function<void()> after_unload);
    void discard_pending_host_at(Web::HTML::CrossProcessId navigable_id, WebContentPage&);
    void dispatch_next_beforeunload_group(HistoryOperation&);
    void complete_unload_cancelation(HistoryOperation&, Web::HTML::HistoryStepResult);
    void dispatch_descendant_unload_task(Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id);
    void complete_descendant_unload_task(Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id);
    void dispatch_crash_recovery_changing_job(HistoryOperation&, NonnullRefPtr<WebContentPage>, Web::HTML::HistoryObjectLengthAndIndex, Function<void()> on_complete);
    void complete_history_jobs_after_crash(HistoryOperation&, Vector<Web::HTML::CrossProcessId> changing_jobs, Vector<Web::HTML::CrossProcessId> nonchanging_updates);
    void finish_deferred_history_operation_after_crash_recovery(Web::HTML::CrossProcessId operation_id);
    ApplyHistoryStepJobs create_apply_history_step_jobs(Web::HTML::CrossProcessId operation_id);
    void run_direct_history_operation(HistoryOperation&);
    void traverse_the_history_by_a_delta_at_queue_position(HistoryOperation&, Web::TraverseByDeltaHistoryOperationParameters const&);
    void perform_a_navigation_api_traversal_at_queue_position(HistoryOperation&, Web::NavigationAPITraverseHistoryOperationParameters const&);
    void finalize_a_same_document_navigation(HistoryOperation&, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&);
    void enqueue_browser_history_traversal(Web::TraverseToStepHistoryOperationParameters, bool check_for_cancelation, OnHistoryOperationComplete = nullptr);
    void run_browser_history_traversal_at_queue_position(Web::TraverseToStepHistoryOperationParameters, bool check_for_cancelation, u64 sequence_number, Function<void()> on_ready, OnHistoryOperationComplete, NonnullRefPtr<Core::Promise<Empty>>);
    void start_history_operation(HistoryOperation&, NonnullRefPtr<Core::Promise<Empty>>);
    void finalize_a_cross_document_navigation(HistoryOperation&, Web::CrossDocumentNavigationFinalizationHostState);
    void apply_history_step(HistoryOperation&, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement, Optional<Web::Bindings::NavigationType>, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot = {});
    void apply_the_push_or_replace_history_step(HistoryOperation&, i32 step, Web::HTML::HistoryHandlingBehavior, Web::HTML::UserNavigationInvolvement);
    void apply_the_reload_history_step(HistoryOperation&, Web::HTML::UserNavigationInvolvement);
    void apply_the_traverse_history_step(HistoryOperation&, i32 step, Optional<Web::InitiatorSourceSnapshot>, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement);
    void resume_applying_the_traverse_history_step(HistoryOperation&, i32 step, Web::HTML::UserNavigationInvolvement);
    void update_for_navigable_creation_or_destruction(HistoryOperation&);
    void finish_history_operation(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult, Optional<i32> committed_step);
    HistoryOperation* ongoing_browser_history_traversal();

    struct PendingBrowserHistoryTraversal {
        enum class Stage : u8 {
            Queued,
            Running,
        };

        u64 generation { 0 };
        u64 sequence_number { 0 };
        Vector<int> deltas;
        Optional<i32> target_step;
        Optional<Web::HTML::CrossProcessId> operation_id;
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
    u64 m_next_sequence_number { 1 };
    u64 m_next_pending_browser_history_traversal_generation { 1 };
    HashMap<Web::HTML::CrossProcessId, NonnullOwnPtr<HistoryOperation>> m_history_operations;
    Optional<PendingBrowserHistoryTraversal> m_pending_browser_history_traversal;

    // Steps 2-5 of unload-a-document-and-its-descendants for one document, keyed by a generated unload id and
    // coordinated here because the descendant subtrees can be hosted by other processes: a snapshot of the
    // document's descendant navigables, each unloaded in its host process only after its own subtree has
    // unloaded. Once the direct children (the parentless nodes) complete, queue_document_unload_task performs
    // the invoking algorithm's step 6. This can be a changing navigable continuation's final unload task,
    // a close's unload-and-destroy task, or a removed child navigable's unload task.
    struct PendingUnload {
        struct Node {
            Optional<Web::HTML::CrossProcessId> parent_id;
            size_t remaining_children { 0 };
            RefPtr<WebContentPage> endpoint;
            Web::HTML::ChildNavigableDestruction child_navigable_destruction { Web::HTML::ChildNavigableDestruction::No };
            Web::HTML::StopHostingAfterUnload stop_hosting_after_unload { Web::HTML::StopHostingAfterUnload::No };
        };
        HashMap<Web::HTML::CrossProcessId, Node> nodes;
        size_t remaining_root_children { 0 };
        // The history operation whose displaced-endpoint list gates dispatches, when the unload is part of one.
        Optional<Web::HTML::CrossProcessId> operation_id;
        Function<void()> queue_document_unload_task;
    };
    HashMap<Web::HTML::CrossProcessId, PendingUnload> m_pending_unloads;

    // Pages that hold this tab, and host none of it, in a process holding part of a tab this tab opened.
    Vector<NonnullRefPtr<WebContentPage>> m_opener_pages;

    struct BeforeunloadGroup {
        NonnullRefPtr<WebContentPage> endpoint;
        Vector<Web::HTML::CrossProcessId> navigable_ids;
    };
    struct PendingBeforeunloadCheck {
        Vector<BeforeunloadGroup> groups;
        RefPtr<WebContentPage> dispatched_endpoint;
        Web::HTML::UnloadPromptShown unload_prompt_shown { Web::HTML::UnloadPromptShown::No };
        Function<void(Web::HTML::HistoryStepResult, Web::HTML::UnloadPromptShown)> on_complete;
    };
    HashMap<Web::HTML::CrossProcessId, PendingBeforeunloadCheck> m_pending_beforeunload_checks;
    void dispatch_next_beforeunload_group(Web::HTML::CrossProcessId check_id);
    bool m_displaced_document_unload_pending { false };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#system-visibility-state
    Web::HTML::VisibilityState m_system_visibility_state { Web::HTML::VisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/interaction.html#system-focus
    bool m_has_system_focus { true };

    Optional<Web::HTML::CrossProcessId> m_focused_navigable_id;
};

}
