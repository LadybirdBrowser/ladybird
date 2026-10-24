/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
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
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/HistoryHandlingBehavior.h>
#include <LibWebCommon/HTML/HistoryOperation.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/HTML/ReplicatedNavigableState.h>
#include <LibWebCommon/HTML/VisibilityState.h>

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
    struct HistoryOperationState {
        GC::Ptr<DOM::Document> pending_document {};
        Optional<CrossProcessId> local_target_navigable_id {};
        RefPtr<SessionHistoryEntry> local_target_entry {};
        GC::Ptr<GC::Function<void()>> on_complete {};
        GC::Ptr<OnApplyHistoryStepComplete> on_apply_history_step_complete {};
        GC::Ptr<GC::Function<void()>> on_navigation_api_traversal_rejected {};

        struct ChangingNavigableJob {
            GC::Ptr<ChangingNavigableContinuationState> continuation;
            GC::Ptr<GC::Function<void(HistoryNavigationPopulation)>> continue_population;
        };
        HashMap<CrossProcessId, ChangingNavigableJob> changing_navigable_jobs {};
    };

    CrossProcessId request_history_operation(HistoryOperationParameters);
    CrossProcessId request_history_operation(HistoryOperationParameters, HistoryOperationState&&);
    // Takes back a requested operation the UI process has not started.
    void revoke_history_operation(CrossProcessId operation_id);
    void complete_ui_history_operation(CrossProcessId operation_id, Optional<i32> target_entry_step);
    void did_apply_ui_history_step(CrossProcessId operation_id, HistoryStepResult);
    void reject_ui_navigation_api_traversal(CrossProcessId operation_id);

    void traverse_the_history_by_delta(int delta, DOM::Document& source_document);
    void finalize_same_document_navigation(GC::Ref<LocalNavigable>, NonnullRefPtr<SessionHistoryEntry>, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior, UserNavigationInvolvement, Optional<SessionHistoryEntryPersistedState> previous_entry_persisted_state);

    void run_ui_beforeunload_check(Vector<CrossProcessId> navigable_ids, Optional<SessionHistoryEntryDescriptor> target_entry, Optional<UserNavigationInvolvement> user_involvement_for_navigate_event, UnloadPromptShown, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult, UnloadPromptShown)>>);
    void run_ui_changing_navigable_history_job(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryDescriptor target_entry_descriptor, UserNavigationInvolvement, Optional<Bindings::NavigationType>, Optional<Utf16String> finalized_navigation_id, bool ongoing_navigation_is_traversal, Optional<NavigationSourceSnapshot>, GC::Ref<OnChangingNavigableHistoryStepJobComplete>, Optional<HistoryNavigationPopulation> = {});
    bool resume_history_navigation_population(CrossProcessId operation_id, HistoryNavigationPopulation&&);
    void apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryIdentity target_entry_identity, HistoryObjectLengthAndIndex, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, GC::Ref<OnChangingNavigableContinuationApplied>);
    void unload_displayed_document_and_apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryIdentity target_entry_identity, HistoryObjectLengthAndIndex, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, GC::Ref<OnChangingNavigableContinuationApplied>);
    void clear_ui_changing_navigable_ongoing_traversal(CrossProcessId operation_id, CrossProcessId navigable_id);

private:
    explicit HistoryExecutor(Page&);

    virtual void visit_edges(Cell::Visitor&) override;

    HistoryOperationState* find_history_operation(CrossProcessId operation_id);
    HistoryOperationState& ensure_history_operation(CrossProcessId operation_id);

    struct DequeuedChangingNavigableContinuation {
        GC::Ref<ChangingNavigableContinuationState> continuation;
        GC::Ref<GC::Function<void()>> after_potential_unloads;
    };
    Optional<DequeuedChangingNavigableContinuation> dequeue_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryIdentity target_entry_identity, HistoryObjectLengthAndIndex, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, GC::Ref<OnChangingNavigableContinuationApplied>);

    void enqueue_changing_navigable_continuation(CrossProcessId operation_id, GC::Ref<ChangingNavigableContinuationState>, GC::Ref<OnChangingNavigableHistoryStepJobComplete>);

    GC::Ref<Page> m_page;

    // One record per history operation this process participates in, keyed by the operation id minted by the
    // operation's initiator. Records for operations requested here exist from the request until
    // complete_history_operation; records for operations initiated elsewhere are created by the first UI job
    // that references them.
    HashMap<CrossProcessId, HistoryOperationState> m_history_operations;
};

}
