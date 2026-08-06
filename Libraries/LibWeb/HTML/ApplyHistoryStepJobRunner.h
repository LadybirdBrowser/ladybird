/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

enum class SynchronousNavigation : bool {
    Yes,
    No,
};

enum class ChangingNavigableHistoryStepJobDisposition : u8 {
    // The job ran and enqueued its changing navigable continuation.
    Ready,
    // AD-HOC: The job did not apply to its navigable (for example, the navigable is gone, or a newer navigation
    //         owns it); the rest of the history step continues without this navigable.
    Skipped,
    // AD-HOC: The whole history step is stale; a newer navigation has to win.
    Stale,
};

struct HistoryObjectLengthAndIndex {
    u64 script_history_length;
    u64 script_history_index;
};

enum class InitiatorSandboxingCheckResult : u8 {
    Allowed,
    Disallowed,
};

using OnInitiatorSandboxingCheckComplete = GC::Function<void(InitiatorSandboxingCheckResult)>;
using OnHistoryStepUnloadCancelationComplete = GC::Function<void(HistoryStepResult)>;

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
//
// The per-navigable jobs of apply-the-history-step. The coordinator runs the traversal-wide algorithm and holds its
// variables; these jobs are the parts of the algorithm that must run where a navigable's documents live. Keeping
// them behind this interface keeps the coordinator free of document state, so the jobs can be run in the navigable's
// own process.
class WEB_API ApplyHistoryStepJobRunner {
public:
    virtual ~ApplyHistoryStepJobRunner() = default;

    virtual void start_apply_history_step_operation(u64 operation_id, u64 history_initiation_id, LocalNavigable::NavigationAPIAbortBehavior) = 0;
    virtual void complete_apply_history_step_operation(u64 operation_id) = 0;

    virtual void run_initiator_sandboxing_check_job(u64 history_initiation_id, CrossProcessId initiator_to_check, Vector<CrossProcessId>, GC::Ref<OnInitiatorSandboxingCheckComplete>) = 0;
    virtual void run_history_step_unload_cancelation_job(int target_step, Vector<CrossProcessId>, UserNavigationInvolvement, GC::Ref<OnHistoryStepUnloadCancelationComplete>) = 0;

    // One iteration of "12. For each navigable of changingNavigables, queue a global task ...".
    //
    // NB: The job also claims its navigable ("2. Set navigable's current session history entry to targetEntry." and
    //     "3. Set the ongoing navigation for navigable to "traversal"."). The specification claims every changing
    //     navigable before queueing the first job, but a claim only mutates the navigable being claimed, which
    //     belongs to the process running the job.
    struct ChangingNavigableHistoryStepJob {
        CrossProcessId navigable_id;
        int target_step { 0 };
        UserNavigationInvolvement user_involvement;
        Optional<Bindings::NavigationType> navigation_type;
        SynchronousNavigation synchronous_navigation;
        LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior;
    };
    using OnChangingNavigableHistoryStepJobComplete = GC::Function<void(ChangingNavigableHistoryStepJobDisposition)>;
    virtual void run_changing_navigable_history_step_job(u64 operation_id, ChangingNavigableHistoryStepJob, GC::Ref<OnChangingNavigableHistoryStepJobComplete>) = 0;

    // The "second part" of a changing navigable's job: "12. In both cases, let afterPotentialUnloads be ...",
    // applied to the continuation the job enqueued.
    struct ApplyChangingNavigableHistoryStepContinuation {
        CrossProcessId navigable_id;
        HistoryObjectLengthAndIndex history_object_length_and_index;
        Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api;
        Optional<Bindings::NavigationType> navigation_type;
        LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior;
        UserNavigationInvolvement user_involvement;
    };
    virtual void apply_changing_navigable_history_step_continuation(u64 operation_id, ApplyChangingNavigableHistoryStepContinuation, GC::Ref<GC::Function<void()>> on_complete) = 0;

    // One iteration of "18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task ...".
    virtual void update_nonchanging_navigable_history_step_state(CrossProcessId navigable_id, HistoryObjectLengthAndIndex, GC::Ref<GC::Function<void()>> on_complete) = 0;
};

}
