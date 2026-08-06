/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Function.h>

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

using OnChangingNavigableHistoryStepJobComplete = GC::Function<void(ChangingNavigableHistoryStepJobDisposition)>;

}
