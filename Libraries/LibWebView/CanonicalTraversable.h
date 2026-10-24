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
#include <LibWebCommon/Bindings/Navigation.h>
#include <LibWebCommon/HTML/HistoryOperation.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/Page/PageId.h>
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

class WEBVIEW_API CanonicalTraversable final : public CanonicalNavigable {
    friend class ApplyHistoryStep;

public:
    CanonicalTraversable(Web::HTML::CrossProcessId id, NonnullRefPtr<CanonicalSessionHistoryEntry> initial_history_entry);
    virtual ~CanonicalTraversable() override;

    virtual bool is_top_level_traversable() const override { return true; }

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

    // Apply-the-history-step coordination. Operations serialize on the traversable's session history traversal
    // queue; the algorithm runs here and dispatches its per-navigable jobs to the processes hosting the documents.
    void enqueue_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters, RefPtr<WebContentPage> requesting_page);
    // Appends plain algorithm steps; a requested traversal defers its target resolution to its queued position, the
    // way the specification's queued steps do, and then starts its operation at that position.
    void append_history_queue_steps(SessionHistoryTraversalSteps);
    void run_history_operation_at_queue_position(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters, RefPtr<WebContentPage> requesting_page, u64 sequence_number, RefPtr<CanonicalSessionHistoryEntry> target_entry, NonnullRefPtr<Core::Promise<Empty>>);
    u64 next_sequence_number() { return m_next_sequence_number++; }
    void abandon_history_operations();

    RefPtr<WebContentPage> page_hosting(CanonicalNavigable const&) const;

    void revoke_history_operation(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id);
    void did_receive_beforeunload_check_result(WebContentPage& source_page, Web::HTML::CrossProcessId check_id, Web::HTML::CheckIfUnloadingIsCanceledResult, Web::HTML::UnloadPromptShown);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
    Optional<Web::HTML::CrossProcessId> check_if_unloading_is_canceled(Vector<Web::HTML::CrossProcessId> navigables_that_need_before_unload, RefPtr<CanonicalSessionHistoryEntry> target_entry, Optional<Web::HTML::UserNavigationInvolvement> user_involvement_for_navigate_event, RefPtr<WebContentPage> skipped_endpoint, Web::HTML::UnloadPromptShown, Function<void(Web::HTML::CheckIfUnloadingIsCanceledResult, Web::HTML::UnloadPromptShown)> on_complete);
    bool is_handing_navigable_to_another_page(CanonicalNavigable const&) const;
    void did_receive_changing_navigable_history_job_ready(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition, bool update_only, Optional<Web::HTML::CrossProcessId> target_document_id);
    void did_finish_history_navigation_params_creation(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation);
    void did_receive_descendant_unload_task_complete(WebContentPage& source_page, Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id);
    void did_receive_child_navigable_unload_request(WebContentPage& source_page, Web::HTML::CrossProcessId navigable_id);
    void did_receive_changing_navigable_continuation_applied(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::HostedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state);
    void did_receive_nonchanging_navigable_history_state_updated(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id);

    CanonicalNavigable& insert(NonnullRefPtr<WebContentPage> reporting_page, CanonicalNavigable& parent, CanonicalDocument& container_document, Web::HTML::CrossProcessId frame_id, Web::HTML::HostedNavigableState, NonnullRefPtr<CanonicalSessionHistoryEntry> active_session_history_entry, NonnullRefPtr<CanonicalDocument>);
    void adopt_nested_history_for_created_child(CanonicalNavigable const& parent, CanonicalDocument const& container_document, Web::HTML::CrossProcessId child_navigable_id);
    Optional<CanonicalNavigable&> find(Web::HTML::CrossProcessId navigable_id);
    Optional<CanonicalNavigable const&> find(Web::HTML::CrossProcessId navigable_id) const;
    void remove(CanonicalNavigable&);
    void remove_page(WebContentPage&);

    Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigable_graph() const;

    void for_each_hosting_page(Function<void(WebContentPage&)> const&) const;
    void represent_group_in(WebContentClient&);
    void represent_group_everywhere();
    void give_stand_in_its_opener(WebContentPage&);
    void did_switch_browsing_context_group();
    bool is_representing_page(WebContentPage const& page) const
    {
        return any_of(m_representing_pages, [&](auto const& representing_page) { return representing_page.ptr() == &page; });
    }
    void forget_representing_page(WebContentPage&);
    void discard_representing_pages();
    void for_each_page_representing(CanonicalNavigable const&, Function<void(WebContentPage&)> const&) const;
    bool hosts(CanonicalNavigable const&, WebContentPage const&) const;
    bool represents(CanonicalNavigable const&, WebContentPage const&) const;
    bool page_hosts_any(WebContentPage const&) const;
    bool page_populates_a_history_entry_for_local_file(WebContentPage const&, ByteString const& path) const;
    void stop_hosting_in_page(CanonicalNavigable&, NonnullRefPtr<WebContentPage>);
    void release_page_if_unused(NonnullRefPtr<WebContentPage>);
    void end_history_jobs(CanonicalNavigable const&, CanonicalDocument const* populated_document = nullptr);

    Optional<ViewImplementation&> view() const;
    void set_view(Badge<ViewImplementation>, ViewImplementation&);
    RefPtr<WebContentPage> display_page() const;
    ErrorOr<NonnullRefPtr<WebContentPage>> obtain_page_to_host_traversable(RefPtr<WebContentClient> host);
    void set_page_standing_in_for_lost_document(Badge<ViewImplementation>, NonnullRefPtr<WebContentPage>);
    void did_activate_document_in(Badge<CanonicalNavigable>, WebContentPage& host);
    void remove_child_navigables_of(CanonicalNavigable&, CanonicalDocument const&);
    void did_lose_page(WebContentPage&, WebContentProcessLost);
    void did_not_receive_reply(OwedReply const&);

    TraversableSessionHistory const& session_history() const { return m_session_history; }
    NonnullRefPtr<CanonicalSessionHistoryEntry> session_history_entry_for(CanonicalNavigable const&, Web::HTML::SameDocumentNavigationEntry const&) const;
    size_t effective_current_session_history_step_index() const;

    StorageJar& session_storage() { return *m_session_storage; }
    void clone_session_storage_from(CanonicalTraversable const&);

    // Fired after every canonical session-history mutation, once the history has reached its post-mutation state.
    Function<void()> on_session_history_changed;

    bool is_created_by_web_content() const { return m_is_created_by_web_content; }
    void set_is_created_by_web_content(bool value) { m_is_created_by_web_content = value; }

    Web::HTML::VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(Web::HTML::VisibilityState);

    bool has_system_focus() const { return m_has_system_focus; }
    void set_has_system_focus(bool, RefPtr<WebContentPage> requesting_page);

    Optional<Web::HTML::CrossProcessId> const& focused_navigable_id() const { return m_focused_navigable_id; }
    void set_focused_navigable(CanonicalNavigable&, WebContentPage& requesting_page);
    RefPtr<WebContentPage> focused_navigable_host() const;
    Web::DevicePixelPoint focused_navigable_host_offset() const;
    Web::DevicePixelPoint local_root_offset(CanonicalNavigable const&) const;

    Optional<BrowserHistoryTraversalDiagnostic> browser_history_traversal_for_testing() const;
    // The entries of the synchronous navigations of a navigable whose finalization is queued.
    Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> queued_same_document_session_history_entries(CanonicalNavigable const&) const;
    ByteString queued_same_document_session_history_entries_for_debug() const;

    void reload();
    void stop_loading();
    static CanonicalTraversable& create_a_new_top_level_traversable(Web::HTML::CrossProcessId id, Optional<CanonicalNavigable&> opener, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::CrossProcessId document_id);
    static bool is_origin_held_by_a_document(URL::Origin const&);
    static CanonicalTraversable* traversable_containing(Web::HTML::CrossProcessId navigable_id);
    static CanonicalNavigable* navigable_with_active_browsing_context(CanonicalBrowsingContext const&);
    static void remove_from_user_agent_top_level_traversable_set(CanonicalTraversable&);
    bool update_session_history_entry_navigation_api_state(CanonicalNavigable&, Web::HTML::SessionHistoryEntryIdentity const&, Web::HTML::StorageSerializationRecord navigation_api_state);
    bool update_session_history_entry_scroll_restoration_mode(CanonicalNavigable&, Web::HTML::SessionHistoryEntryIdentity const&, Web::HTML::ScrollRestorationMode scroll_restoration_mode);
    bool update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable&, Web::HTML::SessionHistoryEntryIdentity const&, Utf16String navigable_target_name);
    bool set_session_history_entry_document_state_reload_pending(CanonicalNavigable const&, Utf16String const& navigation_api_key, bool reload_pending);
    Optional<i32> append_nested_history(CanonicalNavigable const& parent_navigable, CanonicalDocumentState& parent_document_state, Web::HTML::CrossProcessId child_navigable_id);
    bool remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id);
    void traverse_the_history_by_delta(int delta, CheckForCancelation, Function<void()> on_ready = nullptr);
    void traverse_the_history_to_step(i32 step, CheckForCancelation, Function<void()> on_ready = nullptr);
    void reconstruct_the_history_to_step(i32 step);
    ErrorOr<URL::URL> restore_session_history_from_ui_snapshot(SessionHistorySnapshot);
    void recover_from_web_content_process_crash();
    void reset_session_history_for_testing(Web::HTML::SessionHistoryEntryDescriptor);
    bool initialize_session_history_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor>, Vector<i32> used_steps, size_t current_used_step_index);

    static StringView browser_history_traversal_stage_to_string(BrowserHistoryTraversalDiagnostic::Stage);

private:
    ViewImplementation* m_view { nullptr };
    void session_history_changed();
    void check_if_unloading_is_canceled(ApplyHistoryStep&, Vector<Web::HTML::CrossProcessId> navigables_that_need_before_unload, NonnullRefPtr<CanonicalSessionHistoryEntry> target_entry, Web::HTML::UserNavigationInvolvement user_involvement_for_navigate_event, Function<void(Web::HTML::CheckIfUnloadingIsCanceledResult)> on_complete);
    void queue_navigation_api_state_clear_task(ApplyHistoryStep&, ChangingNavigable&);
    bool run_changing_navigable_history_step_job(ApplyHistoryStep&, ChangingNavigable&);
    void apply_changing_navigable_history_step_continuation(ApplyHistoryStep&, ChangingNavigable&);
    bool update_nonchanging_navigable_history_step_state(ApplyHistoryStep&, Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex);

    HistoryOperation* find_history_operation(Web::HTML::CrossProcessId operation_id);
    ApplyHistoryStep* find_history_step_run(Web::HTML::CrossProcessId operation_id);
    RefPtr<CanonicalNavigation> finalized_navigation(HistoryOperation const&) const;
    bool update_session_history_entry_persisted_state(CanonicalNavigable&, Web::HTML::SessionHistoryEntryPersistedState const&);
    void add_history_operation_completion_endpoint(HistoryOperation&, NonnullRefPtr<WebContentPage>);
    void did_fail_history_navigation_population(HistoryOperation&, ChangingNavigable&);
    void continue_history_navigation_population(HistoryOperation&, ChangingNavigable&);
    void send_changing_navigable_continuation_task(HistoryOperation&, ChangingNavigable&);
    void send_displayed_document_unload_task(HistoryOperation&, ChangingNavigable&);
    void deactivate_a_document_for_cross_document_navigation(HistoryOperation&, ChangingNavigable&);
    void did_activate_history_entry(HistoryOperation&, ChangingNavigable&, NonnullRefPtr<WebContentPage> source_page, Web::HTML::HostedNavigableState activated_navigable_state);
    void unload_a_document_and_its_descendants(CanonicalNavigable&, RefPtr<WebContentPage> continuing_endpoint, Web::HTML::ChildNavigableDestruction, Function<void()> queue_document_unload_task);
    void unload_the_documents_of_child_navigables(CanonicalNavigable const&, Function<void()> after_child_navigable_unloads);
    void run_document_unload_task(RefPtr<WebContentPage>, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChildNavigableDestruction, Web::HTML::StopHostingAfterUnload, Function<void()> after_unload);
    void complete_document_unload_task(Web::HTML::CrossProcessId unload_id);
    void stand_in_for_lost_document(CanonicalNavigable&);
    RefPtr<WebContentPage> changing_job_endpoint(CanonicalNavigable const&, CanonicalDocumentState const& target_document_state) const;
    NonnullRefPtr<WebContentPage> changing_job_endpoint(ChangingNavigable const&) const;
    void run_history_operation(HistoryOperation&);
    void traverse_the_history_by_a_delta_at_queue_position(HistoryOperation&, Web::TraverseByDeltaHistoryOperationParameters const&);
    void perform_a_navigation_api_traversal_at_queue_position(HistoryOperation&, Web::NavigationAPITraverseHistoryOperationParameters const&);
    void finalize_a_same_document_navigation(HistoryOperation&, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&);
    void enqueue_browser_history_traversal(i32 target_step, bool check_for_cancelation);
    void run_browser_history_traversal_at_queue_position(i32 target_step, bool check_for_cancelation, u64 sequence_number, Function<void()> on_ready, NonnullRefPtr<Core::Promise<Empty>>);
    void did_attempt_to_populate_the_history_entry_document(WebContentPage& source_page, Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const&);
    void finalize_a_cross_document_navigation(HistoryOperation&);
    void apply_history_step(HistoryOperation&, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement, Optional<Web::Bindings::NavigationType>, Optional<Web::HTML::NavigationSourceSnapshot> source_snapshot_params = {});
    void apply_the_push_or_replace_history_step(HistoryOperation&, i32 step, Web::HTML::HistoryHandlingBehavior, Web::HTML::UserNavigationInvolvement);
    void apply_the_reload_history_step(HistoryOperation&, Web::HTML::UserNavigationInvolvement);
    void apply_the_traverse_history_step(HistoryOperation&, i32 step, Optional<Web::HTML::NavigationSourceSnapshot>, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement);
    void resume_applying_the_traverse_history_step(HistoryOperation&, i32 step, Web::HTML::UserNavigationInvolvement);
    void update_for_navigable_creation_or_destruction(HistoryOperation&);
    void finish_history_operation(Web::HTML::CrossProcessId operation_id, Optional<Web::HTML::HistoryStepResult> = {});
    void did_not_apply_reload(Web::HTML::CrossProcessId navigable_id);
    HistoryOperation* ongoing_browser_history_traversal();

    void run_browser_ui_traversal_at_queue_position(Function<Optional<i32>()> select_target_step, CheckForCancelation, Function<void()> on_ready, NonnullRefPtr<Core::Promise<Empty>>);
    void supersede_browser_history_traversal(HistoryOperation&, i32 target_step, Function<void()> on_ready);

    RefPtr<CanonicalSessionHistoryEntry> session_history_entry_named(CanonicalNavigable const&, Function<bool(CanonicalSessionHistoryEntry const&)> const& matches);
    RefPtr<CanonicalSessionHistoryEntry> session_history_entry_named(CanonicalNavigable const&, Web::HTML::SessionHistoryEntryIdentity const&);
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
    HashMap<Web::HTML::CrossProcessId, NonnullOwnPtr<HistoryOperation>> m_history_operations;
    // The traversal from the browser's UI among the operations, if any.
    HistoryOperation* m_browser_history_traversal { nullptr };
    RefPtr<WebContentPage> m_page_standing_in_for_lost_document;

    // What runs once the page hosting a document has run the task step 6 of unload a document and its descendants
    // queues for it, keyed by the ID the page replies with.
    HashMap<Web::HTML::CrossProcessId, Function<void()>> m_document_unload_tasks;

    // Pages that hold this tab, and host none of it, in a process holding part of a tab of its group.
    Vector<NonnullRefPtr<WebContentPage>> m_representing_pages;

    struct BeforeunloadGroup {
        NonnullRefPtr<WebContentPage> endpoint;
        Vector<Web::HTML::CrossProcessId> navigable_ids;
        bool is_traversable_host { false };
    };
    struct PendingBeforeunloadCheck {
        RefPtr<CanonicalSessionHistoryEntry> target_entry;
        Optional<Web::HTML::UserNavigationInvolvement> user_involvement_for_navigate_event;
        Vector<BeforeunloadGroup> groups;
        Web::HTML::UnloadPromptShown unload_prompt_shown { Web::HTML::UnloadPromptShown::No };
        Function<void(Web::HTML::CheckIfUnloadingIsCanceledResult, Web::HTML::UnloadPromptShown)> on_complete;
    };
    HashMap<Web::HTML::CrossProcessId, PendingBeforeunloadCheck> m_pending_beforeunload_checks;
    void dispatch_next_beforeunload_group(Web::HTML::CrossProcessId check_id);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-created-by-web-content
    bool m_is_created_by_web_content { false };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#system-visibility-state
    Web::HTML::VisibilityState m_system_visibility_state { Web::HTML::VisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/interaction.html#system-focus
    bool m_has_system_focus { true };

    Optional<Web::HTML::CrossProcessId> m_focused_navigable_id;
};

}
