/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CompositorState.h>
#include <Compositor/ContextState.h>
#include <LibCompositing/DisplayList/CanvasSurfaceRegistry.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibTest/TestCase.h>

class NullWebContentClient final : public Compositor::CompositorStateWebContentClient {
public:
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const&) override { }
    virtual void dispatch_key_event_to_web_content(u64, Web::KeyEvent const&) override { }
    virtual void request_rendering_update() override { }
    virtual void rendering_opportunity(Web::CompositorContextId, i64, double) override { }
    virtual void clock_tick(Web::CompositorContextId, i64, double, Vector<Web::CompositorScrollOffset> const&) override { }
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) override { }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool) override { }
};

static Web::UniqueNodeID const document_id { 7 };

// A display list of a page whose viewport cannot be scrolled, so it records no scroll node.
static void install_display_list_without_scroll_nodes(Compositor::ContextState& context)
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto visual_context_tree = builder.finish();
    auto display_list = Compositing::DisplayList::create(visual_context_tree);
    display_list->set_async_scrolling_metadata({ .document_id = document_id, .viewport_rect = { 0, 0, 800, 600 } });
    context.install_display_list_update(display_list, move(visual_context_tree), {});
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
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, canvas_surface_registry };
    install_display_list_without_scroll_nodes(context);

    auto result = context.handle_pinch_event(pinch_event(1.0));
    EXPECT(result.accepted);
    EXPECT(result.frame_to_present.has_value());
}

TEST_CASE(pinch_zoom_of_a_page_without_scroll_nodes_survives_a_display_list_update)
{
    NullWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, canvas_surface_registry };
    install_display_list_without_scroll_nodes(context);

    // Zoom all the way in, then take a display list the main thread recorded before it saw the pinch.
    EXPECT(context.handle_pinch_event(pinch_event(4.0)).accepted);
    install_display_list_without_scroll_nodes(context);

    // The zoom is still at its maximum, so zooming in further does nothing.
    EXPECT(!context.handle_pinch_event(pinch_event(1.0)).accepted);
}

TEST_CASE(wheel_pans_the_zoomed_visual_viewport_of_a_page_without_scroll_nodes)
{
    NullWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, canvas_surface_registry };
    install_display_list_without_scroll_nodes(context);

    Web::MouseEvent wheel_event;
    wheel_event.type = Web::MouseEvent::Type::MouseWheel;
    wheel_event.position = { 400, 300 };
    wheel_event.wheel_delta_y = 50;
    wheel_event.wheel_delta_precision = Web::WheelDeltaPrecision::Precise;

    // Before the page is zoomed in, there is nothing for the wheel to pan.
    EXPECT(!context.handle_wheel_event(wheel_event).accepted);

    EXPECT(context.handle_pinch_event(pinch_event(1.0)).accepted);
    auto result = context.handle_wheel_event(wheel_event);
    EXPECT(result.accepted);
    EXPECT(result.frame_to_present.has_value());

    // The main thread adopts the pan as a scroll of the document's viewport, at half the delta since the zoom is 2x.
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.document_id, document_id);
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    auto const& scroll_offset = updates.scroll_offsets.first();
    EXPECT_EQ(scroll_offset.stable_node_id.node_id, document_id);
    EXPECT_EQ(scroll_offset.stable_node_id.kind, Web::AsyncScrollNodeKind::Viewport);
    EXPECT_EQ(scroll_offset.unadopted_scroll_delta, Gfx::FloatPoint(0, 25));
}
