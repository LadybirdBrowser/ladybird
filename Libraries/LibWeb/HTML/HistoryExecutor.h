/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/HTML/VisibilityState.h>

namespace Web::HTML {

struct ChangingNavigableContinuationState;
struct PopulateSessionHistoryEntryDocumentOutput;

// The renderer's share of session history operations for the documents a Page hosts. The UI process owns the
// session history traversal queue and the canonical session history; this object keeps the records for operations
// requested from this page and the state those operations retain between the jobs the UI process dispatches here.
class WEB_API HistoryExecutor final : public GC::Cell {
    GC_CELL(HistoryExecutor, GC::Cell);
    GC_DECLARE_ALLOCATOR(HistoryExecutor);

public:
    using OnHistoryOperationReady = GC::Function<void(Web::HistoryOperationReadyResult)>;
    using OnHistoryOperationPreSteps = GC::Function<void(Optional<Web::ReconstructedChildNavigation>, GC::Ref<OnHistoryOperationReady>)>;
    struct HistoryOperationState {
        GC::Ptr<DOM::Document> pending_document {};
        GC::Ptr<LocalNavigable> expected_ongoing_navigation_navigable {};
        Optional<Utf16String> expected_ongoing_navigation_id {};
        GC::Ptr<SourceSnapshotParams> source_snapshot_params {};
        Optional<NavigationSourceSnapshot> serialized_source_snapshot_params {};
        Optional<CrossProcessId> local_target_navigable_id {};
        RefPtr<SessionHistoryEntry> local_target_entry {};
        GC::Ptr<OnHistoryOperationPreSteps> pre_steps {};
        GC::Ptr<OnApplyHistoryStepComplete> on_apply_complete {};
        GC::Ptr<OnApplyHistoryStepComplete> on_complete {};

        // State retained between a changing job and its continuation.
        HashTable<CrossProcessId> claimed_navigables_awaiting_continuation {};
        HashMap<CrossProcessId, GC::Ref<ChangingNavigableContinuationState>> changing_navigable_continuations {};
        HashMap<CrossProcessId, GC::Ref<GC::Function<void(HistoryNavigationPopulation)>>> pending_populations {};

        bool expected_ongoing_navigation_was_superseded() const;
    };

    void request_history_operation(HistoryOperationParameters);
    void request_history_operation(HistoryOperationParameters, HistoryOperationState);
    void handle_ui_history_operation_started(CrossProcessId operation_id, Optional<Web::ReconstructedChildNavigation>, GC::Ref<OnHistoryOperationReady>);
    void complete_ui_history_operation(CrossProcessId operation_id, HistoryStepResult, Optional<i32> committed_step);

    void traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document = {});
    void finalize_same_document_navigation(GC::Ref<LocalNavigable>, NonnullRefPtr<SessionHistoryEntry>, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior, UserNavigationInvolvement, Optional<SessionHistoryEntryPersistedState> previous_entry_persisted_state);

    void run_ui_history_step_beforeunload_check(Vector<CrossProcessId> navigable_ids, UnloadPromptShown, GC::Ref<GC::Function<void(HistoryStepResult, UnloadPromptShown)>>);
    void run_ui_changing_navigable_history_job(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryDescriptor target_entry, UserNavigationInvolvement, Optional<Bindings::NavigationType>, bool superseded_by_newer_navigation, GC::Ref<OnChangingNavigableHistoryStepJobComplete>, Optional<HistoryNavigationPopulation> = {});
    bool resume_history_navigation_population(CrossProcessId operation_id, HistoryNavigationPopulation&&);
    void prepare_ui_changing_navigable_for_unload(CrossProcessId operation_id, CrossProcessId navigable_id, GC::Ref<GC::Function<void()>> on_complete);
    void apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, HistoryObjectLengthAndIndex, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, VisibilityState, UnloadDisplayedDocument, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>>);

private:
    explicit HistoryExecutor(Page&);

    virtual void visit_edges(Cell::Visitor&) override;

    HistoryOperationState* find_history_operation(CrossProcessId operation_id);
    HistoryOperationState& ensure_history_operation(CrossProcessId operation_id);

    // One iteration of "12. For each navigable of changingNavigables, queue a global task ...".
    struct ChangingNavigableHistoryStepJob {
        CrossProcessId operation_id;
        CrossProcessId navigable_id;
        NonnullRefPtr<SessionHistoryEntry> target_entry;
        UserNavigationInvolvement user_involvement;
        Optional<Bindings::NavigationType> navigation_type;
        bool superseded_by_newer_navigation { false };
        Optional<NavigationSourceSnapshot> source_snapshot;
        Optional<HistoryNavigationPopulation> population;
    };
    struct LocalChangingNavigableHistoryStepJobResult {
        ChangingNavigableHistoryStepJobDisposition disposition;
        GC::Ptr<ChangingNavigableContinuationState> continuation;
    };
    using OnLocalChangingNavigableHistoryStepJobComplete = GC::Function<void(LocalChangingNavigableHistoryStepJobResult)>;
    struct LocalApplyChangingNavigableHistoryStepContinuation {
        HistoryObjectLengthAndIndex history_object_length_and_index;
        Vector<NonnullRefPtr<SessionHistoryEntry>> entries_for_navigation_api;
        VisibilityState system_visibility_state { VisibilityState::Hidden };
    };
    bool run_changing_navigable_history_step_job_impl(ChangingNavigableHistoryStepJob, GC::Ptr<SourceSnapshotParams>, GC::Ptr<DOM::Document> pending_document, GC::Ref<OnLocalChangingNavigableHistoryStepJobComplete>);
    void apply_changing_navigable_history_step_continuation_impl(GC::Ref<ChangingNavigableContinuationState>, LocalApplyChangingNavigableHistoryStepContinuation, UnloadDisplayedDocument, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>> on_complete);

    GC::Ref<Page> m_page;

    // One record per history operation this process participates in, keyed by the operation id minted by the
    // operation's initiator. Records for operations requested here exist from the request until
    // complete_history_operation; records for operations initiated elsewhere are created by the first UI job
    // that references them.
    HashMap<CrossProcessId, HistoryOperationState> m_history_operations;
};

}
