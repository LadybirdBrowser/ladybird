/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/CompositorState.h>
#include <Compositor/ContextState.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibTest/TestCase.h>

class NullWebContentClient final : public Compositor::CompositorStateWebContentClient {
public:
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const&) override { }
    virtual void dispatch_key_event_to_web_content(u64, Web::KeyEvent const&) override { }
    virtual void dispatch_pinch_event_to_web_content(u64, Web::PinchEvent const&) override { }
    virtual void request_rendering_update() override { }
    virtual void rendering_opportunity(Web::CompositorContextId, i64, double) override { }
    virtual void clock_tick(Web::CompositorContextId, i64, double, Vector<Web::CompositorScrollOffset> const&) override { }
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) override { }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool) override { }
    virtual Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const override { return Compositing::CanvasSurfaceOwnerId { 1 }; }
};

static Web::UniqueNodeID const document_id { 7 };

// A display list of a page whose viewport cannot be scrolled, so it records no scroll node.
static void install_display_list_without_scroll_nodes(Compositor::ContextState& context, u64 adopted_async_scroll_sequence = 0)
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto visual_context_tree = builder.finish();
    auto display_list = Compositing::DisplayList::create(visual_context_tree);
    display_list->set_async_scrolling_metadata({ .document_id = document_id, .viewport_rect = { 0, 0, 800, 600 } });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    scroll_state_snapshot.set_adopted_async_scroll_sequence(adopted_async_scroll_sequence);
    context.install_display_list_update(display_list, move(visual_context_tree), move(scroll_state_snapshot));
}

static Web::PinchEvent pinch_event(double scale_delta)
{
    Web::PinchEvent event;
    event.position = { 400, 300 };
    event.scale_delta = scale_delta;
    return event;
}

TEST_CASE(pinch_zooms_a_page_without_scroll_nodes)
{
    NullWebContentClient client;
    Compositor::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, Compositor::OwnedCanvasSurfaces { canvas_surface_registry, client.canvas_surface_owner_id() } };
    install_display_list_without_scroll_nodes(context);

    auto result = context.handle_pinch_event(pinch_event(1.0));
    EXPECT(result.accepted);
    EXPECT(result.frame_to_present.has_value());
}

TEST_CASE(pinch_zoom_of_a_page_without_scroll_nodes_survives_a_display_list_update)
{
    NullWebContentClient client;
    Compositor::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, Compositor::OwnedCanvasSurfaces { canvas_surface_registry, client.canvas_surface_owner_id() } };
    install_display_list_without_scroll_nodes(context);

    // Zoom all the way in, then take a display list the main thread recorded before it saw the pinch.
    EXPECT(context.handle_pinch_event(pinch_event(4.0)).accepted);
    install_display_list_without_scroll_nodes(context);

    // The zoom is still at its maximum, so zooming in further does nothing.
    EXPECT(!context.handle_pinch_event(pinch_event(1.0)).accepted);
}

TEST_CASE(pinch_zoom_is_reported_on_the_visual_viewport)
{
    NullWebContentClient client;
    Compositor::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, Compositor::OwnedCanvasSurfaces { canvas_surface_registry, client.canvas_surface_owner_id() } };
    install_display_list_without_scroll_nodes(context);

    // Zooming in 2x around the middle of the viewport moves the visual viewport to the middle of the page.
    EXPECT(context.handle_pinch_event(pinch_event(1.0)).accepted);
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    auto const& scroll_offset = updates.scroll_offsets.first();
    EXPECT_EQ(scroll_offset.stable_node_id.node_id, document_id);
    EXPECT(scroll_offset.stable_node_id.kind == Web::AsyncScrollNodeKind::VisualViewport);
    EXPECT_EQ(scroll_offset.visual_viewport_scale, 2.0f);
    EXPECT_EQ(scroll_offset.compositor_scroll_offset, Gfx::FloatPoint(200, 150));
    EXPECT_EQ(scroll_offset.unadopted_scroll_delta, Gfx::FloatPoint(200, 150));
}

TEST_CASE(pinch_zoom_is_presented_until_the_main_thread_adopts_it)
{
    NullWebContentClient client;
    Compositor::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, Compositor::OwnedCanvasSurfaces { canvas_surface_registry, client.canvas_surface_owner_id() } };
    install_display_list_without_scroll_nodes(context);

    EXPECT(context.handle_pinch_event(pinch_event(4.0)).accepted);
    auto updates = context.take_pending_async_scroll_updates();

    // A display list recorded before the main thread adopted the zoom keeps it at its maximum.
    install_display_list_without_scroll_nodes(context, updates.sequence - 1);
    EXPECT(!context.handle_pinch_event(pinch_event(1.0)).accepted);

    // Once the main thread has adopted the zoom, its display list says where the visual viewport is, here reset since.
    install_display_list_without_scroll_nodes(context, updates.sequence);
    EXPECT(context.handle_pinch_event(pinch_event(1.0)).accepted);
}

TEST_CASE(wheel_pans_the_zoomed_visual_viewport_of_a_page_without_scroll_nodes)
{
    NullWebContentClient client;
    Compositor::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, Compositor::OwnedCanvasSurfaces { canvas_surface_registry, client.canvas_surface_owner_id() } };
    install_display_list_without_scroll_nodes(context);

    Web::MouseEvent wheel_event;
    wheel_event.type = Web::MouseEvent::Type::MouseWheel;
    wheel_event.position = { 400, 300 };
    wheel_event.wheel_delta_y = 50;
    wheel_event.wheel_delta_precision = Web::WheelDeltaPrecision::Precise;

    // Before the page is zoomed in, there is nothing for the wheel to pan.
    EXPECT(!context.handle_wheel_event(wheel_event).accepted);

    EXPECT(context.handle_pinch_event(pinch_event(1.0)).accepted);
    (void)context.take_pending_async_scroll_updates();
    auto result = context.handle_wheel_event(wheel_event);
    EXPECT(result.accepted);
    EXPECT(result.frame_to_present.has_value());

    // The main thread adopts the pan as a scroll of the document's visual viewport, at half the delta at 2x zoom.
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.document_id, document_id);
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    auto const& scroll_offset = updates.scroll_offsets.first();
    EXPECT_EQ(scroll_offset.stable_node_id.node_id, document_id);
    EXPECT_EQ(scroll_offset.stable_node_id.kind, Web::AsyncScrollNodeKind::VisualViewport);
    EXPECT_EQ(scroll_offset.unadopted_scroll_delta, Gfx::FloatPoint(0, 25));
}
