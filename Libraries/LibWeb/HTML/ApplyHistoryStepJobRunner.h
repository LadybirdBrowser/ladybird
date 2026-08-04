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
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

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

    virtual void run_changing_navigable_history_step_job(u64 operation_id, ChangingNavigableHistoryStepJob, GC::Ref<OnChangingNavigableHistoryStepJobComplete>) = 0;

    virtual void apply_changing_navigable_history_step_continuation(u64 operation_id, ApplyChangingNavigableHistoryStepContinuation, GC::Ref<GC::Function<void()>> on_complete) = 0;

    // One iteration of "18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task ...".
    virtual void update_nonchanging_navigable_history_step_state(CrossProcessId navigable_id, HistoryObjectLengthAndIndex, GC::Ref<GC::Function<void()>> on_complete) = 0;
};

}
