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
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

struct PopulateSessionHistoryEntryDocumentOutput;

// The state a changing navigable's history step job retains for its continuation.
struct ChangingNavigableContinuationState : public GC::Cell {
    GC_CELL(ChangingNavigableContinuationState, GC::Cell);
    GC_DECLARE_ALLOCATOR(ChangingNavigableContinuationState);

    GC::Ptr<DOM::Document> displayed_document;
    Optional<UniqueNodeID> displayed_document_id;
    RefPtr<SessionHistoryEntry> target_entry;
    GC::Ptr<LocalNavigable> navigable;
    bool update_only = false;
    Optional<Bindings::NavigationType> navigation_type;
    UserNavigationInvolvement user_involvement { UserNavigationInvolvement::None };

    GC::Ptr<DOM::Document> pending_document;
    GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> population_output;
    GC::Ptr<DOM::Document> resolved_document;
    Optional<URL::Origin> old_origin;

    virtual void visit_edges(Cell::Visitor&) override;
};

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

    HistoryOperationState* find_history_operation(CrossProcessId operation_id);
    HistoryOperationState& ensure_history_operation(CrossProcessId operation_id);

private:
    explicit HistoryExecutor(Page&);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Page> m_page;

    // One record per history operation this process participates in, keyed by the operation id minted by the
    // operation's initiator. Records for operations requested here exist from the request until
    // complete_history_operation; records for operations initiated elsewhere are created by the first UI job
    // that references them.
    HashMap<CrossProcessId, HistoryOperationState> m_history_operations;
};

}
