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
#define ENUMERATE_INPUT_ROUTING_DECISIONS(X)                                                       \
    X(ContextNotPresenting, "context-not-presenting")                                              \
    X(NoAsyncScrollingState, "no-async-scrolling-state")                                           \
    X(BlockingWheelEventListeners, "blocking-wheel-event-listeners")                               \
    X(NoScrollNode, "no-scroll-node")                                                              \
    X(StaleWheelEventListeners, "stale-wheel-event-listeners")                                     \
    X(StepOvertakenByUI, "step-overtaken-by-ui")                                                   \
    X(GestureEndWithNothingToSnap, "gesture-end-with-nothing-to-snap")                             \
    X(SnappedAtGestureEnd, "snapped-at-gesture-end")                                               \
    X(HitTestTargetsOfAnotherVisualContextTree, "hit-test-targets-of-another-visual-context-tree") \
    X(BlockedByMainThreadRegion, "blocked-by-main-thread-region")                                  \
    X(BlockedByWheelEventRegion, "blocked-by-wheel-event-region")                                  \
    X(NoScrollTarget, "no-scroll-target")                                                          \
    X(ScrollerInOtherDocument, "scroller-in-other-document")                                       \
    X(StepWithoutDelta, "step-without-delta")                                                      \
    X(PannedVisualViewport, "panned-visual-viewport")                                              \
    X(Latched, "latched")                                                                          \
    X(LatchedOutermostScrollerToStretch, "latched-outermost-scroller-to-stretch")                  \
    X(ContinuedLatch, "continued-latch")                                                           \
    X(NotOverAScrollbar, "not-over-a-scrollbar")                                                   \
    X(NotPrimaryButton, "not-primary-button")                                                      \
    X(HoveringScrollbarPaintedByCompositor, "hovering-scrollbar-painted-by-compositor")            \
    X(BeganScrollbarDrag, "began-scrollbar-drag")                                                  \
    X(ContinuedScrollbarDrag, "continued-scrollbar-drag")                                          \
    X(EndedScrollbarDrag, "ended-scrollbar-drag")                                                  \
    X(NoScrollbarDrag, "no-scrollbar-drag")                                                        \
    X(LeftDuringScrollbarDrag, "left-during-scrollbar-drag")

enum class InputRoutingDecision : u8 {
#define __ENUMERATE_INPUT_ROUTING_DECISION(name, string) name,
    ENUMERATE_INPUT_ROUTING_DECISIONS(__ENUMERATE_INPUT_ROUTING_DECISION)
#undef __ENUMERATE_INPUT_ROUTING_DECISION
};

WEBCOMMON_API StringView input_routing_decision_name(InputRoutingDecision);

}
