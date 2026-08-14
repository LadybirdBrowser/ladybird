/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibCore/Promise.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/SessionHistory.h>
#include <LibWebView/SessionHistoryTraversalQueue.h>

namespace WebView {

// The per-navigable jobs of apply-the-history-step, run in the process hosting each navigable's documents. Jobs are
// described by ids and entry descriptors, never by live documents.
//
// The caller-supplied job functions must complete every job exactly once, even when the target process is missing or
// has crashed (for example with a Skipped disposition), and must not invoke a job's completion callback after the
// operation that dispatched it has completed.
struct WEBVIEW_API ApplyHistoryStepJobs {

    // "5. If checkForCancelation is true, and the result of checking if unloading is canceled given
    //  navigablesCrossingDocuments, traversable, targetStep, and userInvolvement is not "continue", then return that
    //  result." — beforeunload runs where the documents live.
    struct UnloadCancelationJob {
        Web::HTML::SessionHistoryEntryDescriptor target_entry;
        Vector<Web::HTML::CrossProcessId> navigables_crossing_documents;
        Web::HTML::UserNavigationInvolvement user_involvement { Web::HTML::UserNavigationInvolvement::None };
    };
    Function<void(UnloadCancelationJob, Function<void(Web::HTML::HistoryStepResult)> on_complete)> run_unload_cancelation_job;

    // One iteration of "12. For each navigable of changingNavigables, queue a global task ...". The job claims its
    // navigable and either enqueues a changing navigable continuation in its own process (Ready) or reports why it
    // could not (see Web::HTML::ChangingNavigableHistoryStepJobDisposition).
    struct ChangingNavigableHistoryStepJob {
        Web::HTML::CrossProcessId navigable_id;
        // The target entry selected from canonical session history. The receiving process retains this exact entry
        // instead of resolving a target step against its local session history projection.
        Web::HTML::SessionHistoryEntryDescriptor target_entry;
        Web::HTML::UserNavigationInvolvement user_involvement { Web::HTML::UserNavigationInvolvement::None };
        Optional<Web::Bindings::NavigationType> navigation_type;
        Web::HTML::LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior { Web::HTML::LocalNavigable::NavigationAPIAbortBehavior::Abort };
    };
    Function<bool(ChangingNavigableHistoryStepJob const&)> select_changing_navigable_history_step_job_endpoint;
    Function<void(ChangingNavigableHistoryStepJob, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete)> run_changing_navigable_history_step_job;

    // The "second part" of a changing navigable's job ("12. In both cases, let afterPotentialUnloads be ..."),
    // applied to the continuation the job enqueued in its own process.
    struct ApplyChangingNavigableHistoryStepContinuation {
        Web::HTML::CrossProcessId navigable_id;
        Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index;
        Vector<Web::HTML::SessionHistoryEntryDescriptor> entries_for_navigation_api;
    };
    Function<void(ApplyChangingNavigableHistoryStepContinuation, Function<void()> on_complete)> apply_changing_navigable_history_step_continuation;

    // One iteration of "18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task ...".
    Function<void(Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex, Function<void()> on_complete)> update_nonchanging_navigable_history_step_state;
};

// The apply-history-step state a traversable navigable shares across runs.
struct TraversableApplyHistoryStepState {
    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-running-nested-apply-history-step
    bool running_nested_apply_history_step { false };

    // AD-HOC: Concurrent runs share the current-step commit: a run commits its target step only if no newer run (a
    //         synchronous navigation that jumped the queue while it was paused) has committed one first, so the
    //         current step cannot move backwards past a newer run's commit.
    //         See https://github.com/whatwg/html/issues/12576.
    u64 generation_counter { 0 };
    u64 committed_generation { 0 };
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
//
// One run of the apply-the-history-step algorithm, coordinated from the UI process. The algorithm's traversal-wide
// variables live on this object; the per-navigable jobs run through caller-supplied functions in the processes
// hosting the documents, and their completion callbacks resume the algorithm at the phase that dispatched them.
//
// NB: The specification's sourceSnapshotParams argument never appears here: it lives in the initiating process
//     together with initiatorToCheck's document, and the jobs that need either run in that process.
class WEBVIEW_API ApplyHistoryStep
    : public Weakable<ApplyHistoryStep> {
    AK_MAKE_NONCOPYABLE(ApplyHistoryStep);
    AK_MAKE_NONMOVABLE(ApplyHistoryStep);

public:
    ApplyHistoryStep(
        TraversableSessionHistory& session_history,
        CanonicalNavigable& traversable_navigable,
        SessionHistoryTraversalQueue& session_history_traversal_queue,
        TraversableApplyHistoryStepState& traversable_state,
        ApplyHistoryStepJobs jobs,
        i32 step,
        bool check_for_cancelation,
        Optional<Web::HTML::CrossProcessId> initiator_to_check,
        Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot,
        Web::HTML::UserNavigationInvolvement user_involvement,
        Optional<Web::Bindings::NavigationType> navigation_type,
        Function<void(Web::HTML::HistoryStepResult)> on_complete);
    ~ApplyHistoryStep();

    void apply_the_history_step();

    bool completed() const { return m_completed; }
    // The step this run committed as the current session history step, if it was the run that committed.
    Optional<i32> committed_step() const { return m_committed_step; }

private:
    // The algorithm's steps. Asynchronous work resumes the algorithm by entering the next phase (or re-entering the
    // current one) from its completion callback.
    void get_changing_and_nonchanging_navigables();
    void run_changing_navigable_jobs();
    void process_changing_navigable_continuations();
    void update_nonchanging_navigables();
    void set_current_session_history_step();

    void changing_navigable_job_completed(Web::HTML::CrossProcessId, Web::HTML::ChangingNavigableHistoryStepJobDisposition);
    void return_result(Web::HTML::HistoryStepResult);
    CanonicalNavigable* find_navigable(Web::HTML::CrossProcessId);

    // The traversable navigable the algorithm runs against: its canonical session history, its navigable tree, its
    // session history traversal queue, and the apply-history-step state it shares across runs.
    TraversableSessionHistory& m_session_history;
    CanonicalNavigable& m_traversable_navigable;
    SessionHistoryTraversalQueue& m_session_history_traversal_queue;
    TraversableApplyHistoryStepState& m_traversable_state;
    ApplyHistoryStepJobs m_jobs;

    // The algorithm's arguments.
    i32 const m_step;
    // NB: The HTML Standard spells this algorithm argument "checkForCancelation".
    bool const m_check_for_cancelation;
    Optional<Web::HTML::CrossProcessId> const m_initiator_to_check;
    Optional<Web::InitiatorSourceSnapshot> const m_initiator_source_snapshot;
    Web::HTML::UserNavigationInvolvement const m_user_involvement;
    Optional<Web::Bindings::NavigationType> const m_navigation_type;
    Function<void(Web::HTML::HistoryStepResult)> m_on_complete;

    // The algorithm's variables.
    u64 const m_generation;
    i32 m_target_step { 0 };
    Vector<Web::HTML::CrossProcessId> m_changing_navigables;
    Vector<Web::HTML::CrossProcessId> m_nonchanging_navigables_that_still_need_updates;
    size_t m_completed_change_jobs { 0 };
    // NB: The continuation states live in the processes that ran the jobs; this queue holds the navigables whose
    //     continuations are ready to be applied.
    Vector<Web::HTML::CrossProcessId> m_changing_navigable_continuations;
    HashTable<Web::HTML::CrossProcessId> m_navigables_that_must_wait_before_handling_sync_navigation;
    Optional<u64> m_synchronous_navigation_steps_to_jump_through;
    size_t m_completed_nonchanging_jobs { 0 };

    // The synchronous navigation steps this run is paused on ("running nested apply history step" is true).
    RefPtr<Core::Promise<Empty>> m_running_synchronous_navigation_steps;

    Optional<i32> m_committed_step;
    bool m_completed { false };
};

}
