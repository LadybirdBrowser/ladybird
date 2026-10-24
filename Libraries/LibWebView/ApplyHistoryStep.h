/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibCore/Promise.h>
#include <LibWebCommon/Bindings/NavigationType.h>
#include <LibWebCommon/HTML/ApplyHistoryStep.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/HistoryOperation.h>
#include <LibWebCommon/HTML/NavigationSourceSnapshot.h>
#include <LibWebCommon/HTML/UserNavigationInvolvement.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/SessionHistory.h>
#include <LibWebView/SessionHistoryTraversalQueue.h>

namespace WebView {

// One of a run's changingNavigables: its history step job, and the changing navigable continuation the job enqueues.
// NB: The process running the job retains the rest of the continuation's state. What waits on the job looks the record
//     up again by its operation and navigable, which name it for as long as its run lasts.
struct WEBVIEW_API ChangingNavigable {
    AK_ALLOC_WITH_KMALLOC;

    ChangingNavigable(CanonicalNavigable& navigable, NonnullRefPtr<CanonicalSessionHistoryEntry> target_entry)
        : id(navigable.id())
        , target_entry(move(target_entry))
        , m_navigable(navigable.make_weak_ptr())
    {
    }

    CanonicalNavigable& navigable() const
    {
        VERIFY(m_navigable);
        return *m_navigable;
    }

    bool has_pending_job() const { return !job_ended; }

    Web::HTML::CrossProcessId id;

    // The process running the job retains this exact entry instead of resolving a target step against its local session
    // history projection. The run looks it up again before it is activated, and follows a sync navigation that changed
    // it (whatwg/html#12961).
    NonnullRefPtr<CanonicalSessionHistoryEntry> target_entry;
    // Whether targetEntry's document state's reload pending was true when the job claimed it, and whether that reload
    // bypasses the HTTP cache. Populating the document sets both to false.
    bool target_entry_reload_pending { false };
    bool target_entry_reload_bypasses_cache { false };
    // The target entry as the job claimed it, for the processes the job runs in.
    Web::HTML::SessionHistoryEntryDescriptor target_entry_descriptor() const;

    // Nothing more comes from the process running the job, and it counts towards completedChangeJobs.
    bool job_ended { false };
    // The run's part of the continuation, which the process running the job applies with the rest.
    struct Continuation {
        Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index;
        Vector<Web::HTML::SessionHistoryEntryDescriptor> entries_for_navigation_api;
    };
    Optional<Continuation> continuation;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#changing-navigable-continuation-state
    // NB: Its target entry and navigable are the record's. The process running the job reports the rest.
    RefPtr<CanonicalDocument> displayed_document;
    bool update_only { false };
    // targetEntry's document, as the job left it.
    Optional<Web::HTML::CrossProcessId> target_document_id;

    RefPtr<NavigationLoader> population_loader;
    RefPtr<CanonicalDocument> populated_document;

private:
    WeakPtr<CanonicalNavigable> m_navigable;
};

// The apply-history-step state a traversable navigable shares across runs.
struct TraversableApplyHistoryStepState {
    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-running-nested-apply-history-step
    bool running_nested_apply_history_step { false };

    // AD-HOC: Concurrent runs share the current-step commit: a run commits its target step only if no newer run (a
    //         synchronous navigation that jumped the queue while it was paused) has moved the current step first, so
    //         the current step cannot move backwards past a newer run's commit. A newer run whose commit moves nothing
    //         (a synchronous replace re-commits the step it started from) leaves the paused run's own commit intact.
    //         See https://github.com/whatwg/html/issues/12576.
    u64 generation_counter { 0 };
    u64 committed_generation { 0 };
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
//
// One run of the apply-the-history-step algorithm, coordinated from the UI process. The algorithm's traversal-wide
// variables live on this object; the traversable runs the per-navigable jobs in the processes hosting the documents,
// and their completions resume the algorithm at the phase that dispatched them. Each job reports to the run exactly
// once, including after its process is lost.
class WEBVIEW_API ApplyHistoryStep {
    AK_MAKE_NONCOPYABLE(ApplyHistoryStep);
    AK_MAKE_NONMOVABLE(ApplyHistoryStep);

public:
    AK_ALLOC_WITH_KMALLOC;

    ApplyHistoryStep(
        CanonicalTraversable& traversable,
        HistoryOperation& operation,
        Web::HTML::CrossProcessId operation_id,
        u64 operation_sequence_number,
        i32 step,
        bool check_for_cancelation,
        Optional<Web::HTML::CrossProcessId> initiator_to_check,
        Optional<Web::HTML::NavigationSourceSnapshot> source_snapshot_params,
        Web::HTML::UserNavigationInvolvement user_involvement,
        Optional<Web::Bindings::NavigationType> navigation_type);
    ~ApplyHistoryStep();

    void apply_the_history_step();
    // The run's operation finished. Its pending jobs end, and it calls none of its jobs from then on.
    void abandon();

    // The step this run committed as the current session history step, if it was the run that committed.
    Optional<i32> committed_step() const { return m_committed_step; }

    HistoryOperation& operation() const { return m_operation; }
    Web::HTML::UserNavigationInvolvement user_involvement() const { return m_user_involvement; }
    Optional<Web::Bindings::NavigationType> navigation_type() const { return m_navigation_type; }
    Optional<Web::HTML::NavigationSourceSnapshot> const& source_snapshot_params() const { return m_source_snapshot_params; }

    Vector<NonnullOwnPtr<ChangingNavigable>> const& changing_navigables() const { return m_changing_navigables; }
    ChangingNavigable* changing_navigable(Web::HTML::CrossProcessId) const;

    void changing_navigable_job_completed(ChangingNavigable&, Web::HTML::ChangingNavigableHistoryStepJobDisposition);
    void nonchanging_navigable_updated();
    // The run goes on once its continuations are processed again.
    void end_changing_navigable_job(ChangingNavigable&);
    void process_changing_navigable_continuations();

private:
    void get_changing_and_nonchanging_navigables();
    void run_changing_navigable_jobs();
    void update_nonchanging_navigables();
    void set_current_session_history_step();
    void append_the_claimed_target_entry_again(CanonicalNavigable&, CanonicalSessionHistoryEntry& claimed_target_entry);

    void set_ongoing_navigation_to_traversal(CanonicalNavigable&, CanonicalSessionHistoryEntry const&);
    void return_result(Web::HTML::HistoryStepResult);

    CanonicalTraversable& m_traversable;
    TraversableSessionHistory& m_session_history;
    SessionHistoryTraversalQueue& m_session_history_traversal_queue;
    TraversableApplyHistoryStepState& m_traversable_state;

    HistoryOperation& m_operation;
    Web::HTML::CrossProcessId const m_operation_id;
    u64 const m_operation_sequence_number;

    // The algorithm's arguments.
    // NB: A run that appends its claimed target entry again moves to that entry's step.
    i32 m_step;
    // NB: The HTML Standard spells this algorithm argument "checkForCancelation".
    bool const m_check_for_cancelation;
    Optional<Web::HTML::CrossProcessId> const m_initiator_to_check;
    Optional<Web::HTML::NavigationSourceSnapshot> const m_source_snapshot_params;
    Web::HTML::UserNavigationInvolvement const m_user_involvement;
    Optional<Web::Bindings::NavigationType> const m_navigation_type;

    // The algorithm's variables.
    i32 m_target_step { 0 };
    Vector<NonnullOwnPtr<ChangingNavigable>> m_changing_navigables;
    Vector<Web::HTML::CrossProcessId> m_nonchanging_navigables_that_still_need_updates;
    size_t m_completed_change_jobs { 0 };
    Vector<ChangingNavigable*> m_changing_navigable_continuations;
    // AD-HOC: One continuation is applied at a time, so synchronous navigations queued by its application are in the
    //         traversal queue before the next continuation checks for them.
    ChangingNavigable* m_continuation_being_applied { nullptr };
    HashTable<Web::HTML::CrossProcessId> m_navigables_that_must_wait_before_handling_sync_navigation;
    size_t m_completed_nonchanging_jobs { 0 };

    // AD-HOC: The generation that orders the runs' commits of the current step (whatwg/html#12576).
    u64 m_generation;
    // How far the run lets synchronous navigation steps jump.
    Optional<u64> m_synchronous_navigation_steps_to_jump_through;

    // The synchronous navigation steps this run is paused on ("running nested apply history step" is true).
    RefPtr<Core::Promise<Empty>> m_running_synchronous_navigation_steps;

    Optional<i32> m_committed_step;
};

}
