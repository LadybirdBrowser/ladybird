/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibWeb/Bindings/NavigationType.h>
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

// The "second part" of a changing navigable's job: "12. In both cases, let afterPotentialUnloads be ...". The
// navigable identifies the continuation parked by the first part within the same operation.
struct ApplyChangingNavigableHistoryStepContinuation {
    CrossProcessId navigable_id;
    HistoryObjectLengthAndIndex history_object_length_and_index;
    Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api;
    Optional<Bindings::NavigationType> navigation_type;
    LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior;
    UserNavigationInvolvement user_involvement;
};

}
