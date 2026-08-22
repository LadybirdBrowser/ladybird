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
#include <LibWeb/HTML/HistoryOperation.h>
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
    static GC::Ref<LocalTraversableNavigable> create_a_new_top_level_traversable(GC::Ref<Page>, GC::Ptr<BrowsingContext> opener, Utf16String target_name, Optional<CrossProcessId> initial_document_state_id = {});
    static GC::Ref<LocalTraversableNavigable> create_a_fresh_top_level_traversable(GC::Ref<Page>, URL::URL const& initial_navigation_url, DocumentResource, CrossProcessId initial_document_state_id);

    virtual ~LocalTraversableNavigable() override;

    virtual bool is_top_level_traversable() const override;

    u64 session_history_entry_count() const { return m_session_history_entry_count; }

    VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(VisibilityState);

    bool is_created_by_web_content() const { return m_is_created_by_web_content; }
    void set_is_created_by_web_content(bool value) { m_is_created_by_web_content = value; }

    using OnHistoryOperationReady = GC::Function<void(Web::HistoryOperationReadyResult)>;
    using OnHistoryOperationPreSteps = GC::Function<void(Optional<Web::ReconstructedChildNavigation>, GC::Ref<OnHistoryOperationReady>)>;
    struct HistoryOperationState {
        GC::Ptr<DOM::Document> pending_document {};
        GC::Ptr<LocalNavigable> expected_ongoing_navigation_navigable {};
        Optional<Utf16String> expected_ongoing_navigation_id {};
        GC::Ptr<SourceSnapshotParams> source_snapshot_params {};
        Optional<CrossProcessId> local_target_navigable_id {};
        RefPtr<SessionHistoryEntry> local_target_entry {};
        GC::Ptr<OnHistoryOperationPreSteps> pre_steps {};
        GC::Ptr<OnApplyHistoryStepComplete> on_apply_complete {};
        GC::Ptr<OnApplyHistoryStepComplete> on_complete {};

        // In-flight job parking: claims and continuations held between a changing job and its continuation or the
        // operation's completion. Operations initiated elsewhere (browser UI, another process) carry only this
        // part; their record is created by the first job that references them.
        HashMap<CrossProcessId, LocalNavigable::NavigationAPIAbortBehavior> claimed_navigables_awaiting_continuation {};
        HashMap<CrossProcessId, GC::Ref<ChangingNavigableContinuationState>> changing_navigable_continuations {};
    };
    void request_history_operation(HistoryOperationParameters);
    void request_history_operation(HistoryOperationParameters, HistoryOperationState);
    void handle_ui_history_operation_started(CrossProcessId operation_id, Optional<Web::ReconstructedChildNavigation>, GC::Ref<OnHistoryOperationReady>);
    void run_ui_history_step_unload_cancelation_job(CrossProcessId operation_id, SessionHistoryEntryDescriptor target_entry, Vector<CrossProcessId> navigables_crossing_documents, UserNavigationInvolvement, GC::Ref<GC::Function<void(HistoryStepResult)>>);
    void run_ui_changing_navigable_history_job(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryDescriptor target_entry, UserNavigationInvolvement, Optional<Bindings::NavigationType>, LocalNavigable::NavigationAPIAbortBehavior, bool superseded_by_newer_navigation, GC::Ref<OnChangingNavigableHistoryStepJobComplete>);
    void apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, HistoryObjectLengthAndIndex, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>>);
    void update_nonchanging_navigable_history_step_state(CrossProcessId navigable_id, HistoryObjectLengthAndIndex, GC::Ref<GC::Function<void()>> on_complete);
    void complete_ui_history_operation(CrossProcessId operation_id, HistoryStepResult, Optional<i32> committed_step, u64 session_history_entry_count);

    void finalize_same_document_navigation(GC::Ref<LocalNavigable>, NonnullRefPtr<SessionHistoryEntry>, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior, UserNavigationInvolvement, Optional<SessionHistoryEntryPersistedState> previous_entry_persisted_state);
    void traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document = {});
    void continue_navigation_at_population(NavigationPopulationRequest, NavigationPopulationResult);
    bool adopt_canonical_id_for_child_created_during_history_reconstruction(LocalNavigable& parent, LocalNavigable& child);
    bool route_child_created_during_history_reconstruction(LocalNavigable& parent, LocalNavigable& child, Web::ReconstructedChildNavigation);
    void reset_session_history_for_testing();

    enum class PromptToUnload : bool {
        No,
        Yes,
    };
    void close_top_level_traversable(PromptToUnload = PromptToUnload::Yes);
    void definitely_close_top_level_traversable(PromptToUnload = PromptToUnload::Yes);
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
        NonnullRefPtr<SessionHistoryEntry> target_entry;
        UserNavigationInvolvement user_involvement;
        Optional<Bindings::NavigationType> navigation_type;
        LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior;
        bool superseded_by_newer_navigation { false };
    };
    struct LocalChangingNavigableHistoryStepJobResult {
        ChangingNavigableHistoryStepJobDisposition disposition;
        GC::Ptr<ChangingNavigableContinuationState> continuation;
    };
    using OnLocalChangingNavigableHistoryStepJobComplete = GC::Function<void(LocalChangingNavigableHistoryStepJobResult)>;
    struct LocalApplyChangingNavigableHistoryStepContinuation {
        HistoryObjectLengthAndIndex history_object_length_and_index;
        Vector<NonnullRefPtr<SessionHistoryEntry>> entries_for_navigation_api;
    };
    bool run_changing_navigable_history_step_job_impl(ChangingNavigableHistoryStepJob, GC::Ptr<SourceSnapshotParams>, GC::Ptr<DOM::Document> pending_document, GC::Ref<OnLocalChangingNavigableHistoryStepJobComplete>);
    void apply_changing_navigable_history_step_continuation_impl(GC::Ref<ChangingNavigableContinuationState>, LocalApplyChangingNavigableHistoryStepContinuation, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>> on_complete);

    void check_if_unloading_is_canceled(Vector<GC::Root<LocalNavigable>> navigables_that_need_before_unload, GC::Ptr<LocalTraversableNavigable> traversable, RefPtr<SessionHistoryEntry> target_entry, Optional<UserNavigationInvolvement> user_involvement_for_navigate_events, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback);

    // WebContent needs the canonical top-level entry count synchronously for is_script_closable().
    u64 m_session_history_entry_count { 1 };

    // One record per history operation this process participates in, keyed by the operation id minted by the
    // operation's initiator. Records for operations requested here exist from the request until
    // complete_history_operation; records for operations initiated elsewhere are created by the first UI job
    // that references them.
    HashMap<CrossProcessId, HistoryOperationState> m_history_operations;

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
