/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
enum class CheckIfUnloadingIsCanceledResult {
    CanceledByBeforeUnload,
    CanceledByNavigate,
    Continue,
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
// They return "initiator-disallowed", "canceled-by-beforeunload", "canceled-by-navigate", or
// "applied".
enum class HistoryStepResult {
    InitiatorDisallowed,
    CanceledByBeforeUnload,
    CanceledByNavigate,
    Applied,
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

// AD-HOC: A same-document traversal yields to a newer navigation of its navigable.
enum class TraversalYieldsTo : u8 {
    Nothing,
    // The UI process admitted a navigation after the traversal.
    AdmittedNavigation,
    // The navigable's process started a navigation before it learned of the traversal.
    UnadmittedNavigation,
};

enum class UnloadPromptShown : bool {
    No,
    Yes,
};

enum class UnloadDisplayedDocument : bool {
    No,
    Yes,
};

enum class ChildNavigableDestruction : bool {
    No,
    Yes,
};

enum class StopHostingAfterUnload : bool {
    No,
    Yes,
};

struct HistoryObjectLengthAndIndex {
    u64 script_history_length;
    u64 script_history_index;
};

}
