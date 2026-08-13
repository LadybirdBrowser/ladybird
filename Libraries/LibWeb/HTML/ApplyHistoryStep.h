/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Function.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
// They return "initiator-disallowed", "canceled-by-beforeunload", "canceled-by-navigate", or
// "applied".
enum class HistoryStepResult {
    InitiatorDisallowed,
    CanceledByBeforeUnload,
    CanceledByNavigate,
    // AD-HOC: This is an internal result used when WebContent no longer has the requested page.
    CanceledByMissingPage,
    // INTEROP: This is an internal result for browser UI handling and is not one of the results
    //          returned by the HTML Standard's apply the history step algorithm.
    CanceledPendingNavigation,
    // AD-HOC: An internal result for when the canonical session history has no entry matching the
    //         requested operation (for example, a navigation API traversal to a pruned entry).
    NoMatchingEntry,
    Applied,
};
using OnApplyHistoryStepComplete = GC::Function<void(HistoryStepResult)>;

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

using OnChangingNavigableHistoryStepJobComplete = GC::Function<void(ChangingNavigableHistoryStepJobDisposition)>;

}
