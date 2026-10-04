/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Web::Compositor {

// What a navigable hands its compositor context for one frame. The frame owns everything its messages carry, so it
// can be handed to the compositor from any thread.
struct CompositorFrame {
    // A newly recorded display list, with the visual context tree, resources and scroll state it paints with.
    struct DisplayListUpdate {
        NonnullRefPtr<Compositing::DisplayList> display_list;
        Compositing::AccumulatedVisualContextTree visual_context_tree;
        Compositing::DisplayListResourceTransaction resource_transaction;
        Compositing::ScrollStateSnapshot scroll_state_snapshot;
    };

    // A new visual context tree for the display list the compositor already has.
    struct VisualContextTreeUpdate {
        Compositing::AccumulatedVisualContextTree visual_context_tree;
        Compositing::DisplayListResourceTransaction resource_transaction;
    };

    // The scroll state of the display list the compositor already has.
    struct ScrollStateUpdate {
        Compositing::ScrollStateSnapshot scroll_state_snapshot;
        Compositing::KeyboardScrollState keyboard_scroll_state;
    };

    // The context the frame is for, which the context handle submitting it stamps.
    Web::CompositorContextId context_id;
    Optional<DisplayListUpdate> display_list_update;
    Optional<VisualContextTreeUpdate> visual_context_tree_update;
    Optional<ScrollStateUpdate> scroll_state_update;
    // Set when the frame is presented once the compositor has applied it.
    Optional<Gfx::IntRect> present_viewport_rect;
};

// Hands finished frames to the compositor. Unlike the rest of a compositor connection, which belongs to the thread that
// made it, a frame sink takes frames from any thread. The messages of one frame reach the compositor together, and
// frames reach it in the order they were submitted.
class WEB_API CompositorFrameSink : public AtomicRefCounted<CompositorFrameSink> {
public:
    virtual ~CompositorFrameSink() = default;

    // Returns false once the compositor can no longer be reached.
    virtual bool submit(CompositorFrame&&) = 0;
};

}
