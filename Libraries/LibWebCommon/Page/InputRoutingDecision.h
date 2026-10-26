/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibWebCommon/Export.h>

namespace Web {

// What decided where an input event went, and why, at one step of its way from the UI to the page.
#define ENUMERATE_INPUT_ROUTING_DECISIONS(X)                                                            \
    X(ContextNotPresenting, "context-not-presenting")                                                   \
    X(NoAsyncScrollingState, "no-async-scrolling-state")                                                \
    X(BlockingWheelEventListeners, "blocking-wheel-event-listeners")                                    \
    X(NoScrollNode, "no-scroll-node")                                                                   \
    X(StaleWheelEventListeners, "stale-wheel-event-listeners")                                          \
    X(StepOvertakenByUI, "step-overtaken-by-ui")                                                        \
    X(GestureEndWithNothingToSnap, "gesture-end-with-nothing-to-snap")                                  \
    X(SnappedAtGestureEnd, "snapped-at-gesture-end")                                                    \
    X(HitTestTargetsOfAnotherVisualContextTree, "hit-test-targets-of-another-visual-context-tree")      \
    X(BlockedByMainThreadRegion, "blocked-by-main-thread-region")                                       \
    X(BlockedByWheelEventRegion, "blocked-by-wheel-event-region")                                       \
    X(NoScrollTarget, "no-scroll-target")                                                               \
    X(ScrollerInOtherDocument, "scroller-in-other-document")                                            \
    X(StepWithoutDelta, "step-without-delta")                                                           \
    X(PannedVisualViewport, "panned-visual-viewport")                                                   \
    X(Latched, "latched")                                                                               \
    X(LatchedOutermostScrollerToStretch, "latched-outermost-scroller-to-stretch")                       \
    X(ContinuedLatch, "continued-latch")                                                                \
    X(HandedToNestedContext, "handed-to-nested-context")                                                \
    X(NestedContextPassedStepOn, "nested-context-passed-step-on")                                       \
    X(NestedContextUnavailable, "nested-context-unavailable")                                           \
    X(NotOverAScrollbar, "not-over-a-scrollbar")                                                        \
    X(NotPrimaryButton, "not-primary-button")                                                           \
    X(HoveringScrollbarPaintedByCompositor, "hovering-scrollbar-painted-by-compositor")                 \
    X(BeganScrollbarDrag, "began-scrollbar-drag")                                                       \
    X(ContinuedScrollbarDrag, "continued-scrollbar-drag")                                               \
    X(EndedScrollbarDrag, "ended-scrollbar-drag")                                                       \
    X(NoScrollbarDrag, "no-scrollbar-drag")                                                             \
    X(LeftDuringScrollbarDrag, "left-during-scrollbar-drag")                                            \
    X(GestureEndAbsorbedByFling, "gesture-end-absorbed-by-fling")                                       \
    X(FlingStarted, "fling-started")                                                                    \
    X(MomentumInterrupted, "momentum-interrupted")                                                      \
    X(NotDispatchedByCompositor, "not-dispatched-by-compositor")                                        \
    X(ConsumedByCompositor, "consumed-by-compositor")                                                   \
    X(ForwardedToPage, "forwarded-to-page")                                                             \
    X(NoAnswerFromCompositor, "no-answer-from-compositor")                                              \
    X(CoalescedIntoLaterEvent, "coalesced-into-later-event")                                            \
    X(DroppedWithoutLocalRoot, "dropped-without-local-root")                                            \
    X(DroppedOutdatedPointerMove, "dropped-outdated-pointer-move")                                      \
    X(ForwardedToRemoteFrame, "forwarded-to-remote-frame")                                              \
    X(AsyncScrollNotAttemptedDisplayListOutdated, "async-scroll-not-attempted-display-list-outdated")   \
    X(AsyncScrollNotAttemptedVisualViewportScaled, "async-scroll-not-attempted-visual-viewport-scaled") \
    X(DefaultActionPerformedByCompositor, "default-action-performed-by-compositor")                     \
    X(NoWheelTarget, "no-wheel-target")                                                                 \
    X(HandedToNestedNavigable, "handed-to-nested-navigable")                                            \
    X(DispatchedWheelEvent, "dispatched-wheel-event")                                                   \
    X(PageCanceledWheelEvent, "page-canceled-wheel-event")                                              \
    X(MainThreadScrolledLatchedScroller, "main-thread-scrolled-latched-scroller")                       \
    X(MainThreadScrolledScroller, "main-thread-scrolled-scroller")                                      \
    X(ScrollKeptByOverscrollBehavior, "scroll-kept-by-overscroll-behavior")                             \
    X(MainThreadScrolledNothing, "main-thread-scrolled-nothing")                                        \
    X(ScrollbarDragDrivenByCompositor, "scrollbar-drag-driven-by-compositor")                           \
    X(FinishedAccepted, "finished-accepted")                                                            \
    X(FinishedHandled, "finished-handled")                                                              \
    X(FinishedCancelled, "finished-cancelled")                                                          \
    X(FinishedDropped, "finished-dropped")

enum class InputRoutingDecision : u8 {
#define __ENUMERATE_INPUT_ROUTING_DECISION(name, string) name,
    ENUMERATE_INPUT_ROUTING_DECISIONS(__ENUMERATE_INPUT_ROUTING_DECISION)
#undef __ENUMERATE_INPUT_ROUTING_DECISION
};

WEBCOMMON_API StringView input_routing_decision_name(InputRoutingDecision);

}
