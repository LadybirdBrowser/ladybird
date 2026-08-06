/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/Export.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageShed.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibGfx/VulkanContext.h>
#endif

namespace Web::HTML {

struct ChangingNavigableContinuationState;

// https://html.spec.whatwg.org/multipage/document-sequences.html#traversable-navigable
class WEB_API LocalTraversableNavigable final : public LocalNavigable {
    GC_CELL(LocalTraversableNavigable, LocalNavigable);
    GC_DECLARE_ALLOCATOR(LocalTraversableNavigable);

public:
    static GC::Ref<LocalTraversableNavigable> create_a_new_top_level_traversable(GC::Ref<Page>, GC::Ptr<BrowsingContext> opener, Utf16String target_name);
    static GC::Ref<LocalTraversableNavigable> create_a_fresh_top_level_traversable(GC::Ref<Page>, URL::URL const& initial_navigation_url, DocumentResource = Empty {});

    virtual ~LocalTraversableNavigable() override;

    virtual bool is_top_level_traversable() const override;

    int current_session_history_step() const { return m_current_session_history_step; }

    // Claims the step number for a new push-type session history entry. Claims are tracked separately from the current
    // step: The current step only advances when an apply-history-step run commits — and several runs can have claimed
    // steps in flight at once. So, computing a new step from the current step alone can hand out a step number that an
    // existing entry already holds. A claim is retired when the run that applies it completes.
    [[nodiscard]] int claim_next_session_history_step();
    void claim_session_history_step(int step);
    void retire_claimed_session_history_step(int step);
    Vector<NonnullRefPtr<SessionHistoryEntry>>& session_history_entries() { return m_session_history_entries; }
    Vector<NonnullRefPtr<SessionHistoryEntry>> const& session_history_entries() const { return m_session_history_entries; }
    struct SessionHistorySnapshot {
        Vector<SessionHistoryEntryDescriptor> top_level_session_history_entries;
        Vector<i32> used_session_history_steps;
        size_t current_used_step_index { 0 };
    };
    enum class SaveActiveEntryPersistedState : bool {
        No,
        Yes,
    };
    SessionHistorySnapshot create_session_history_snapshot(SaveActiveEntryPersistedState = SaveActiveEntryPersistedState::Yes);

    VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(VisibilityState);

    bool is_created_by_web_content() const { return m_is_created_by_web_content; }
    void set_is_created_by_web_content(bool value) { m_is_created_by_web_content = value; }

    HistoryObjectLengthAndIndex get_the_history_object_length_and_index(int) const;

    using OnHistoryOperationReady = GC::Function<void(bool proceed, Optional<i32> step_override, HistoryStepResult abandon_result)>;
    using OnHistoryOperationPreSteps = GC::Function<void(u64 history_initiation_id, GC::Ref<OnHistoryOperationReady>)>;
    struct HistoryOperationState {
        GC::Ptr<DOM::Document> pending_document {};
        GC::Ptr<LocalNavigable> expected_ongoing_navigation_navigable {};
        Optional<Utf16String> expected_ongoing_navigation_id {};
        GC::Ptr<SourceSnapshotParams> source_snapshot_params {};
        GC::Ptr<LocalNavigable> initiator_to_check {};
        Optional<CrossProcessId> finalized_navigable_id {};
        Optional<LocalNavigable::NavigationAPIAbortBehavior> navigation_api_abort_behavior {};
        Optional<int> claimed_step {};
        GC::Ptr<OnHistoryOperationPreSteps> pre_steps {};
        GC::Ptr<OnApplyHistoryStepComplete> on_apply_complete {};
        GC::Ptr<OnApplyHistoryStepComplete> on_complete {};
    };
    void request_history_operation(HistoryOperationParameters);
    void request_history_operation(HistoryOperationParameters, HistoryOperationState);
    void request_synchronous_navigation_history_operation(GC::Ref<LocalNavigable> target_navigable, HistoryOperationParameters);
    void request_synchronous_navigation_history_operation(GC::Ref<LocalNavigable> target_navigable, HistoryOperationParameters, HistoryOperationState);
    void set_history_operation_claimed_step(u64 initiation_id, int step);

    void handle_ui_history_operation_started(u64 operation_id, Optional<u64> initiation_id, GC::Ref<OnHistoryOperationReady>);
    bool run_ui_initiator_sandboxing_check_job(CrossProcessId initiator_to_check_id, Vector<CrossProcessId> const& navigables, u64 initiation_id);
    void run_ui_history_step_unload_cancelation_job(u64 operation_id, int target_step, Vector<CrossProcessId> navigables_crossing_documents, UserNavigationInvolvement, GC::Ref<GC::Function<void(HistoryStepResult)>>);
    void run_ui_changing_navigable_history_job(u64 operation_id, CrossProcessId navigable_id, int target_step, SessionHistoryEntryDescriptor target_entry, UserNavigationInvolvement, Optional<Bindings::NavigationType>, bool synchronous_navigation, Optional<u64> initiation_id, GC::Ref<OnChangingNavigableHistoryStepJobComplete>);
    void apply_ui_changing_navigable_continuation(u64 operation_id, CrossProcessId navigable_id, HistoryObjectLengthAndIndex, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, GC::Ref<GC::Function<void()>>);
    void update_nonchanging_navigable_history_step_state(CrossProcessId navigable_id, HistoryObjectLengthAndIndex, GC::Ref<GC::Function<void()>> on_complete);
    void complete_ui_history_operation(u64 operation_id, HistoryStepResult, Optional<i32> committed_step, Optional<u64> initiation_id);
    bool has_ui_history_operation_in_flight() const { return !m_ui_history_operations.is_empty(); }

    void finalize_same_document_navigation(GC::Ref<LocalNavigable>, NonnullRefPtr<SessionHistoryEntry>, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior, UserNavigationInvolvement);
    void did_complete_finalize_same_document_navigation(u64 operation_id, bool committed, int entry_step, int target_step, HistoryObjectLengthAndIndex);
    int get_the_used_step(int step) const;
    Vector<GC::Root<LocalNavigable>> get_all_local_navigables_that_might_experience_a_cross_document_traversal(int) const;

    Vector<int> get_all_used_history_steps() const;
    void clear_the_forward_session_history();
    void traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document = {});
    bool replace_top_level_session_history_entries_from_ui_process(Vector<SessionHistoryEntryDescriptor>, size_t current_top_level_entry_index, bool allow_reconstructing_current_entry);
    void reset_session_history_for_testing(GC::Ref<GC::Function<void()>> on_complete);

    void close_top_level_traversable();
    void definitely_close_top_level_traversable();
    void destroy_top_level_traversable();

    Utf16String const& window_handle() const { return m_window_handle; }
    void set_window_handle(Utf16String window_handle) { m_window_handle = move(window_handle); }

    [[nodiscard]] GC::Ptr<DOM::Node> currently_focused_area();

    enum class CheckIfUnloadingIsCanceledResult {
        CanceledByBeforeUnload,
        CanceledByNavigate,
        Continue,
    };
    void check_if_unloading_is_canceled(Vector<GC::Root<LocalNavigable>> navigables_that_need_before_unload, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback);

    StorageAPI::StorageShed& storage_shed() { return m_storage_shed; }
    StorageAPI::StorageShed const& storage_shed() const { return m_storage_shed; }

    // https://w3c.github.io/geolocation/#dfn-emulated-position-data
    Geolocation::EmulatedPositionData const& emulated_position_data() const;
    void set_emulated_position_data(Geolocation::EmulatedPositionData data);
    void set_emulated_position_data(Geolocation::CoordinatesData);
    u64 register_emulated_position_data_observer(GC::Ref<GC::Function<void()>>);
    void unregister_emulated_position_data_observer(u64 observer_id);

    void process_screenshot_requests();
    void queue_screenshot_task(Optional<UniqueNodeID> node_id)
    {
        m_screenshot_tasks.enqueue({ node_id });
        set_needs_repaint();
        page().client().request_frame();
    }

private:
    LocalTraversableNavigable(GC::Ref<Page>);

    virtual bool is_traversable() const override { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

    // One iteration of "12. For each navigable of changingNavigables, queue a global task ...".
    struct ChangingNavigableHistoryStepJob {
        CrossProcessId navigable_id;
        int target_step { 0 };
        UserNavigationInvolvement user_involvement;
        Optional<Bindings::NavigationType> navigation_type;
        SynchronousNavigation synchronous_navigation;
        LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior;
    };
    struct LocalChangingNavigableHistoryStepJobResult {
        ChangingNavigableHistoryStepJobDisposition disposition;
        GC::Ptr<ChangingNavigableContinuationState> continuation;
    };
    using OnLocalChangingNavigableHistoryStepJobComplete = GC::Function<void(LocalChangingNavigableHistoryStepJobResult)>;
    struct LocalApplyChangingNavigableHistoryStepContinuation {
        HistoryObjectLengthAndIndex history_object_length_and_index;
        Vector<NonnullRefPtr<SessionHistoryEntry>> entries_for_navigation_api;
        Optional<Bindings::NavigationType> navigation_type;
        LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior;
        UserNavigationInvolvement user_involvement;
    };
    bool run_changing_navigable_history_step_job_impl(ChangingNavigableHistoryStepJob, GC::Ptr<SourceSnapshotParams>, GC::Ptr<DOM::Document> pending_document, GC::Ref<OnLocalChangingNavigableHistoryStepJobComplete>);
    void apply_changing_navigable_history_step_continuation_impl(GC::Ref<ChangingNavigableContinuationState>, LocalApplyChangingNavigableHistoryStepContinuation, GC::Ref<GC::Function<void()>> on_complete);

    void check_if_unloading_is_canceled(Vector<GC::Root<LocalNavigable>> navigables_that_need_before_unload, GC::Ptr<LocalTraversableNavigable> traversable, Optional<int> target_step, Optional<UserNavigationInvolvement> user_involvement_for_navigate_events, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback);

    Vector<NonnullRefPtr<SessionHistoryEntry>> get_session_history_entries_for_the_navigation_api(GC::Ref<LocalNavigable>, int);
    Vector<NonnullRefPtr<SessionHistoryEntry>> get_session_history_entries_for_the_navigation_api(CrossProcessId navigable_id, int);

    [[nodiscard]] bool can_go_back() const;
    [[nodiscard]] bool can_go_forward() const;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    int m_current_session_history_step { 0 };

    // Concurrent apply-history-step operations share the step numbering below. Operations are serialized through
    // the UI-owned session history traversal queue — but a synchronous navigation can jump the queue while another
    // operation is paused (see the "sync navigations jump queue" in the spec), and the synchronous fast path commits
    // outside the queue entirely. See https://github.com/whatwg/html/issues/12576.
    //
    //  - uniqueness: a new step number is claimed past every claimed-but-uncommitted step — never just current step + 1
    //    (claim_next_session_history_step); a claim is retired when its coordinated operation completes.
    //
    //  - integrity: clearing the forward session history spares entries whose steps are claimed by operations still
    //    in flight (clear_the_forward_session_history).
    Vector<int> m_outstanding_claimed_session_history_steps;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
    Vector<NonnullRefPtr<SessionHistoryEntry>> m_session_history_entries;

    // Per-operation state for UI-coordinated apply-history-step runs. An operation is in flight from
    // history_operation_started until complete_history_operation.
    struct UIHistoryOperationState {
        Optional<u64> initiation_id;
        Optional<LocalNavigable::NavigationAPIAbortBehavior> navigation_api_abort_behavior;
        Optional<Bindings::NavigationType> navigation_type;
        Optional<UserNavigationInvolvement> user_involvement;
        HashTable<CrossProcessId> claimed_navigables_awaiting_continuation;
        HashMap<CrossProcessId, GC::Ref<ChangingNavigableContinuationState>> changing_navigable_continuations;
    };
    LocalNavigable::NavigationAPIAbortBehavior resolve_ui_history_operation_abort_behavior(UIHistoryOperationState&, Optional<Bindings::NavigationType>, int target_step);
    HashMap<u64, UIHistoryOperationState> m_ui_history_operations;
    u64 m_next_history_initiation_id { 1 };
    HashMap<u64, HistoryOperationState> m_history_operation_states;

    struct PendingSameDocumentNavigation {
        GC::Ref<LocalNavigable> target_navigable;
        NonnullRefPtr<SessionHistoryEntry> target_entry;
        RefPtr<SessionHistoryEntry> entry_to_replace;
        Optional<int> provisional_claimed_step;
        GC::Ptr<OnHistoryOperationReady> ready;
        Optional<u64> initiation_id;
    };
    void begin_same_document_navigation_finalization(GC::Ref<LocalNavigable>, NonnullRefPtr<SessionHistoryEntry>, RefPtr<SessionHistoryEntry> entry_to_replace, FinalizeSameDocumentNavigationHistoryOperationParameters const&, GC::Ptr<OnHistoryOperationReady> ready, Optional<u64> initiation_id);
    void set_history_object_length_and_index(HistoryObjectLengthAndIndex);
    u64 m_next_same_document_navigation_operation_id { 1 };
    HashMap<u64, PendingSameDocumentNavigation> m_pending_same_document_navigations;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#system-visibility-state
    VisibilityState m_system_visibility_state { VisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-created-by-web-content
    bool m_is_created_by_web_content { false };

    // https://storage.spec.whatwg.org/#traversable-navigable-storage-shed
    // A traversable navigable holds a storage shed, which is a storage shed. A traversable navigable’s storage shed holds all session storage data.
    GC::Ref<StorageAPI::StorageShed> m_storage_shed;

    Utf16String m_window_handle;

    // https://w3c.github.io/geolocation/#dfn-emulated-position-data
    Geolocation::EmulatedPositionData m_emulated_position_data;
    HashMap<u64, GC::Ref<GC::Function<void()>>> m_emulated_position_data_observers;
    u64 m_next_emulated_position_data_observer_id { 0 };

    struct ScreenshotTask {
        Optional<Web::UniqueNodeID> node_id;
    };
    Queue<ScreenshotTask> m_screenshot_tasks;
};

struct BrowsingContextAndDocument {
    GC::Ref<HTML::BrowsingContext> browsing_context;
    GC::Ref<DOM::Document> document;
};

BrowsingContextAndDocument create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page);

template<>
inline bool LocalNavigable::fast_is<LocalTraversableNavigable>() const { return is_traversable(); }

}
