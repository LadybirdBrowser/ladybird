/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/BackingStoreManager.h>
#include <LibWeb/Compositor/CompositorThread.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::Compositor {

class CompositorContextState {
    AK_MAKE_NONCOPYABLE(CompositorContextState);
    AK_MAKE_NONMOVABLE(CompositorContextState);

public:
    struct ViewportScrollbarDrag {
        size_t scrollbar_index { 0 };
        float primary_position { 0 };
        float thumb_grab_position { 0 };
    };

    struct AppliedViewportScrollbarDrag {
        Vector<AsyncScrollOffset> scroll_offsets;
        Gfx::FloatPoint viewport_scroll_offset;
    };

    struct ViewportScrollbarOverlayState {
        Vector<ViewportScrollbar> scrollbars;
        Optional<size_t> hovered_scrollbar_index;
        Optional<size_t> captured_scrollbar_index;
    };

    CompositorContextState(Optional<u64> page_id, CompositorThread::PagePresentationRegistration);
    ~CompositorContextState();

    bool has_presented_bitmap_awaiting_ack() const { return presented_bitmap_id_awaiting_ack.has_value(); }
    AsyncScrollOperationID next_async_scroll_operation_id();
    void record_completed_async_scroll_operation(Optional<AsyncScrollOperationID>);
    bool has_pending_async_scroll_updates() const;
    CompositorThread::PendingAsyncScrollUpdates take_pending_async_scroll_updates();
    void store_pending_async_scroll_offsets(Vector<AsyncScrollOffset> const&, Optional<AsyncScrollOperationID> = {});
    Optional<Gfx::FloatPoint> reapply_pending_async_scroll_offsets(Vector<AsyncScrollOffset> const&);

    void set_async_scrolling_state(AsyncScrollingState&&);
    WheelHitTestResult hit_test_scroll_node_for_wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta) const;
    Optional<size_t> hit_test_viewport_scrollbar(Gfx::FloatPoint position) const;
    Optional<ViewportScrollbarDrag> begin_viewport_scrollbar_drag(Gfx::FloatPoint position);
    Optional<ViewportScrollbarDrag> captured_viewport_scrollbar_drag(Gfx::FloatPoint position);
    Optional<ViewportScrollbarDrag> release_captured_viewport_scrollbar_drag(Gfx::FloatPoint position);
    bool set_hovered_viewport_scrollbar(Optional<size_t>);
    Optional<AppliedViewportScrollbarDrag> apply_viewport_scrollbar_drag(size_t scrollbar_index, float primary_position, float thumb_grab_position);

    ViewportScrollbarOverlayState viewport_scrollbar_overlay_state() const;
    static void paint_viewport_scrollbar_overlay(Gfx::PaintingSurface&, ViewportScrollbarOverlayState const&, Painting::ScrollStateSnapshot const&);
    static Optional<Gfx::FloatPoint> viewport_scroll_offset_from(Vector<AsyncScrollOffset> const&);

    bool presents_to_client { false };
    Optional<u64> page_id;
    Painting::DisplayListResourceStorage display_list_resource_storage;
    RefPtr<Painting::DisplayList> cached_display_list;
    Painting::ScrollStateSnapshot cached_scroll_state_snapshot;
    Vector<ViewportScrollbar> viewport_scrollbars;
    Optional<size_t> hovered_viewport_scrollbar_index;
    Optional<size_t> captured_viewport_scrollbar_index;
    float viewport_scrollbar_thumb_grab_position { 0 };
    AsyncScrollTree async_scroll_tree;
    BackingStoreManager backing_store_manager;
    CompositorThread::PresentationMode presentation_mode { CompositorThread::PresentToUI {} };

    Optional<i32> presented_bitmap_id_awaiting_ack;
    bool is_rasterizing { false };

    bool needs_present { false };
    Gfx::IntRect pending_viewport_rect;
    bool has_deferred_async_scroll_present { false };
    Gfx::IntRect deferred_async_scroll_present_viewport_rect;

    Vector<AsyncScrollOffset> pending_async_scroll_offsets;
    Vector<AsyncScrollOperationID> completed_async_scroll_operation_ids;
    AsyncScrollOperationID next_async_scroll_operation_id_value { 0 };
    Gfx::IntRect async_scrolling_viewport_rect;
    bool has_async_scrolling_state { false };
    bool can_accept_async_wheel_events { false };
    u64 wheel_event_listener_state_generation { 0 };
    WheelRoutingAdmission wheel_routing_admission { WheelRoutingAdmission::NoAsyncScrollingState };
};

}
