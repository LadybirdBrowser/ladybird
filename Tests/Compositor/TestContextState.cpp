/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <Compositor/CompositorState.h>
#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/FramePacer.h>
#include <LibCompositing/DisplayList/DisplayListDamage.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/PausedDebuggerOverlay.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <Tests/LibCompositing/DisplayListTestHelpers.h>

struct TestWebContentClient final : public Compositor::CompositorStateWebContentClient {
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const& event) override
    {
        events.append("mouse_event"_string);
        forwarded_mouse_events.append(event.clone_without_browser_data());
    }
    virtual void dispatch_key_event_to_web_content(u64, Web::KeyEvent const&) override { }
    virtual void request_rendering_update() override { events.append("request_rendering_update"_string); }
    virtual void rendering_opportunity(Web::CompositorContextId, i64, double) override { }
    virtual void clock_tick(Web::CompositorContextId, i64, double, Vector<Web::CompositorScrollOffset> const&) override { }
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) override { events.append("async_scroll_updates"_string); }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool) override { }

    String event_sequence() const { return MUST(String::join(","sv, events)); }

    Vector<String> events;
    Vector<Web::MouseEvent> forwarded_mouse_events;
};

// Importing consumes the send rights a publication carries. Until then they keep IOSurfaceIsInUse reporting the
// surfaces as in use, exactly as they do for the UI process before it imports them.
static Vector<Gfx::SharedImageBuffer> import_shared_images(Vector<Gfx::SharedImage>& shared_images)
{
    Vector<Gfx::SharedImageBuffer> shared_image_buffers;
    for (auto& shared_image : shared_images)
        shared_image_buffers.append(Gfx::SharedImageBuffer::import_from_shared_image(move(shared_image)));
    shared_images.clear();
    return shared_image_buffers;
}

#ifdef AK_OS_MACOS
// The handle a presenting process would hold. Use counts are system-wide, so marking the surface in use through it
// is what the compositor's own handle observes.
static Core::IOSurfaceHandle handle_for_marking_in_use(Gfx::SharedImageBuffer const& shared_image_buffer)
{
    return Core::IOSurfaceHandle::from_ref(shared_image_buffer.iosurface_handle().core_foundation_pointer());
}
#endif

struct TestCompositorClient final : public Compositor::CompositorStateClient {
    struct PresentedFrame {
        Gfx::IntRect content_rect;
        Gfx::IntRect damage_rect;
        i32 bitmap_id { 0 };
    };

    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& shared_images) override
    {
        allocated_bitmap_ids = move(bitmap_ids);
        allocated_shared_image_buffers = import_shared_images(shared_images);
    }

    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& shared_images) override
    {
        allocated_bitmap_ids.extend(bitmap_ids);
        allocated_shared_image_buffers.extend(import_shared_images(shared_images));
        added_bitmap_ids.extend(move(bitmap_ids));
    }

    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) override
    {
        for (auto bitmap_id : bitmap_ids) {
            auto index = allocated_bitmap_ids.find_first_index(bitmap_id);
            VERIFY(index.has_value());
            allocated_bitmap_ids.remove(*index);
            allocated_shared_image_buffers.remove(*index);
        }
        retired_bitmap_ids.extend(move(bitmap_ids));
    }

    virtual void did_present_frame(Web::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id) override
    {
        presented_frames.append({ content_rect, damage_rect, bitmap_id });
    }

    virtual void did_consume_input_event(Web::CompositorContextId, u64 event_id) override
    {
        consumed_input_event_ids.append(event_id);
    }

    virtual void did_not_dispatch_input_event(Web::CompositorContextId, u64 event_id) override
    {
        undispatched_input_event_ids.append(event_id);
    }

    Vector<i32> allocated_bitmap_ids;
    Vector<Gfx::SharedImageBuffer> allocated_shared_image_buffers;
    Vector<i32> added_bitmap_ids;
    Vector<i32> retired_bitmap_ids;
    Vector<PresentedFrame> presented_frames;
    Vector<u64> consumed_input_event_ids;
    Vector<u64> undispatched_input_event_ids;
};

static bool spin_event_loop_until(Core::EventLoop& event_loop, int timeout_in_milliseconds, Function<bool()> condition)
{
    bool timed_out = false;
    auto timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&] { timed_out = true; });
    timeout_timer->start();
    event_loop.spin_until([&] { return timed_out || condition(); });
    return !timed_out;
}

TEST_CASE(caret_blink_phase_is_sampled_from_its_web_content_reset_time)
{
    Compositing::PaintCaret caret {
        .rect = { 1, 2, 1, 10 },
        .color = Gfx::Color::Black,
        .blink_cycle_start_time_ns = 1'000'000'000,
        .should_blink = true,
    };

    EXPECT(Compositing::caret_is_visible_at_time(caret, 1'000'000'000));
    EXPECT(Compositing::caret_is_visible_at_time(caret, 1'499'999'999));
    EXPECT(!Compositing::caret_is_visible_at_time(caret, 1'500'000'000));
    EXPECT(!Compositing::caret_is_visible_at_time(caret, 1'999'999'999));
    EXPECT(Compositing::caret_is_visible_at_time(caret, 2'000'000'000));

    caret.should_blink = false;
    EXPECT(Compositing::caret_is_visible_at_time(caret, NumericLimits<i64>::max()));
}

static NonnullRefPtr<Compositing::DisplayList> make_display_list(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Optional<Gfx::Color> color, Optional<Gfx::Color> surface_clear_color = {}, Compositing::ContextRef context = {})
{
    TestDisplayList command_bytes;
    if (color.has_value()) {
        auto command = Compositing::FillRect { { 0, 0, 4, 4 }, *color, Gfx::CompositingAndBlendingOperator::Normal, Compositing::NO_EFFECT_NODE };
        append_display_list_command(command_bytes, command, command.rect, context);
    }
    return decode_display_list(visual_context_tree, move(command_bytes), surface_clear_color);
}

static Compositing::AccumulatedVisualContextTree make_visual_context_tree()
{
    Compositing::VisualContextTreeTestBuilder builder;
    return builder.finish();
}

static Compositing::AccumulatedVisualContextTree make_scrollable_viewport_visual_context_tree()
{
    Compositing::VisualContextTreeTestBuilder builder;
    builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    return builder.finish();
}

static NonnullRefPtr<Compositing::DisplayList> make_scrollable_viewport_display_list(Compositing::AccumulatedVisualContextTree const& visual_context_tree, bool with_viewport_scrollbar = true, Optional<Compositing::ContextRef> wheel_hit_test_context = {})
{
    TestDisplayList command_bytes;
    Web::UniqueNodeID document_id { 1 };
    Compositing::SpatialNodeIndex scroll_node_index { 1 };
    VERIFY(visual_context_tree.spatial_node_count() > scroll_node_index.value());

    append_display_list_command(
        command_bytes,
        Compositing::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 2 },
            .scroll_node_index = scroll_node_index,
            .parent_scroll_node_index = Compositing::VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Compositing::CompositorScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
            .is_viewport = true,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        });

    if (wheel_hit_test_context.has_value()) {
        append_display_list_command(
            command_bytes,
            Compositing::CompositorWheelHitTestTarget {
                .document_id = document_id,
                .target_scroll_node_index = scroll_node_index,
                .rect = { 0, 0, 100, 100 },
            },
            {},
            *wheel_hit_test_context);
    }

    if (with_viewport_scrollbar) {
        append_display_list_command(
            command_bytes,
            Compositing::CompositorScrollbar {
                .document_id = document_id,
                .scroll_node_index = scroll_node_index,
                .gutter_rect = { 96, 0, 4, 100 },
                .thumb_rect = { 98, 0, 2, 20 },
                .track_rect = { 96, 0, 4, 100 },
                .expanded_gutter_rect = { 92, 0, 8, 100 },
                .expanded_thumb_rect = { 94, 0, 6, 20 },
                .scroll_size = 0.8,
                .expanded_scroll_size = 0.8,
                .min_scroll_offset = 0,
                .max_scroll_offset = 100,
                .thumb_color = Gfx::Color::Black,
                .track_color = Gfx::Color::Transparent,
                .vertical = true,
                .is_painted_by_compositor = true,
                .display_list_paints_enlarged_scrollbar = false,
            });
    }

    return decode_display_list(visual_context_tree, move(command_bytes), {},
        Compositing::DisplayList::AsyncScrollingMetadata {
            .viewport_rect = { 0, 0, 100, 100 },
        });
}

static Web::MouseEvent mouse_event(Web::MouseEvent::Type type, int x, int y, Web::UIEvents::MouseButton button = Web::UIEvents::MouseButton::None)
{
    return {
        .type = type,
        .position = { Web::DevicePixels { x }, Web::DevicePixels { y } },
        .screen_position = {},
        .button = button,
        .buttons = Web::UIEvents::MouseButton::None,
        .modifiers = Web::UIEvents::KeyModifier::Mod_None,
        .wheel_delta_x = 0,
        .wheel_delta_y = 0,
        .click_count = 0,
        .browser_data = nullptr,
        .async_scroll_performed_default_action = false,
    };
}

TEST_CASE(rasterization_clears_damaged_pixels_to_the_canvas_color_in_presentation_backing_stores)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    Compositor::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    auto visual_context_tree = Compositing::VisualContextTreeTestBuilder().finish();
    auto viewport_rect = Gfx::IntRect { 0, 0, 4, 4 };

    context.viewport_size_updated(viewport_rect.size(), Compositing::WindowResizingInProgress::No);
    auto publication = context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed);
    VERIFY(publication.has_value());
    auto imported_backing_stores = import_shared_images(publication->shared_images);

    auto paint_frame = [&](NonnullRefPtr<Compositing::DisplayList> display_list) {
        context.install_display_list_update(move(display_list), visual_context_tree, {});
        context.queue_present_frame({ viewport_rect, viewport_rect });
        EXPECT(context.present_synchronously(display_list_player, nullptr));
    };

    // Paint the first two backing stores red before reusing the first one for a frame with no commands.
    paint_frame(make_display_list(visual_context_tree, Gfx::Color::Red));
    EXPECT(context.acknowledge_presented_bitmap(publication->bitmap_ids[0]));
    paint_frame(make_display_list(visual_context_tree, Gfx::Color::Red));
    paint_frame(make_display_list(visual_context_tree, {}, Gfx::Color::Green));

    auto bitmap = context.latest_rendered_surface()->snapshot_bitmap();
    EXPECT_EQ(bitmap->get_pixel(0, 0), Gfx::Color::Green);

    paint_frame(make_display_list(visual_context_tree, {}));
    bitmap = context.latest_rendered_surface()->snapshot_bitmap();
    EXPECT_EQ(bitmap->get_pixel(0, 0), Gfx::Color::Transparent);
}

struct ContextDrawingVideo {
    static constexpr Compositing::VideoSinkResourceId video_sink_id { 1 };
    static constexpr Media::VideoSinkHandle video_sink_handle { 1 };

    ContextDrawingVideo()
    {
        context.viewport_size_updated(viewport_rect.size(), Compositing::WindowResizingInProgress::No);
        auto publication = context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed);
        VERIFY(publication.has_value());
        imported_backing_stores = import_shared_images(publication.value().shared_images);

        Compositing::DisplayListResourceTransaction resource_transaction;
        resource_transaction.video_sinks.append({ video_sink_id, video_sink_handle });
        context.apply_display_list_resource_transaction(move(resource_transaction));
        TestDisplayList command_bytes;
        auto fill = Compositing::FillRect { { 0, 0, 4, 4 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, Compositing::NO_EFFECT_NODE };
        append_display_list_command(command_bytes, fill, fill.rect);
        auto video = Compositing::DrawVideoFrame { { 8, 8, 4, 4 }, video_sink_id, Gfx::ScalingMode::NearestNeighbor };
        append_display_list_command(command_bytes, video, video.dst_rect);
        context.install_display_list_update(decode_display_list(visual_context_tree, move(command_bytes)), visual_context_tree, {});
        context.queue_present_frame(Compositor::ContextState::PendingFrame::repainting_everything(viewport_rect));
        VERIFY(context.present_synchronously(display_list_player, nullptr));
    }

    Optional<Gfx::IntRect> damage_of_next_frame()
    {
        auto prepared_frame = context.prepare_frame(display_list_player, Compositor::ContextState::PendingFrame::repainting_changes(viewport_rect), nullptr);
        if (!prepared_frame.has_value())
            return {};
        return prepared_frame.value().damage_rect;
    }

    Gfx::IntRect viewport_rect { 0, 0, 16, 16 };
    Gfx::IntRect video_damage_rect { 7, 7, 6, 6 };
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    Compositor::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Compositing::AccumulatedVisualContextTree visual_context_tree { Compositing::VisualContextTreeTestBuilder().finish() };
    Vector<Gfx::SharedImageBuffer> imported_backing_stores;
};

TEST_CASE(a_new_video_frame_damages_only_the_video)
{
    ContextDrawingVideo fixture;
    EXPECT(!fixture.damage_of_next_frame().has_value());

    fixture.context.did_change_video_frame(ContextDrawingVideo::video_sink_handle);
    EXPECT_EQ(fixture.damage_of_next_frame(), fixture.video_damage_rect);
}

TEST_CASE(attaching_a_video_sink_damages_only_the_video)
{
    ContextDrawingVideo fixture;
    auto media_time_writer = MUST(Media::MediaTimeWriter::create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(MUST(Media::MediaTimeReader::create(media_time_writer.buffer()))));

    fixture.context.set_video_sink(ContextDrawingVideo::video_sink_id, sink);
    EXPECT_EQ(fixture.damage_of_next_frame(), fixture.video_damage_rect);

    fixture.context.set_video_sink(ContextDrawingVideo::video_sink_id, sink);
    EXPECT(!fixture.damage_of_next_frame().has_value());
}

TEST_CASE(wheel_hit_testing_ignores_targets_from_a_larger_visual_context_tree)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    Compositing::VisualContextTreeTestBuilder builder;
    auto scroll_node = builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    auto removed_transform = builder.append_transform(scroll_node, Gfx::FloatMatrix4x4::identity());
    auto visual_context_tree = builder.finish();

    context.install_display_list_update(
        make_scrollable_viewport_display_list(visual_context_tree, false, Compositing::ContextRef { removed_transform }),
        visual_context_tree,
        {});

    auto smaller_visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(
        make_scrollable_viewport_display_list(smaller_visual_context_tree, false, Compositing::ContextRef { removed_transform }),
        smaller_visual_context_tree,
        {});

    auto result = context.async_scroll_by(
        Web::UniqueNodeID { 1 },
        { 20, 20 },
        { 0, 10 },
        { 0, 0, 100, 100 },
        Web::WheelDeltaPrecision::Precise, Web::ScrollGesturePhase::None, Web::UIEvents::KeyModifier::Mod_None,
        Compositing::AsyncScrollOperationTracking::No);
    EXPECT(result.enqueue_result.accepted);
}

TEST_CASE(pinch_zoom_copies_the_visual_context_tree_once_per_update)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    for (u64 update = 1; update <= 5; ++update) {
        auto result = context.handle_pinch_event({
            .position = { Web::DevicePixels { 50 }, Web::DevicePixels { 50 } },
            .scale_delta = 0.1,
        });
        EXPECT(result.accepted);
        VERIFY(result.frame_to_present.has_value());
        EXPECT_EQ(result.frame_to_present->forced_damage_rect, (Gfx::IntRect { 0, 0, 100, 100 }));
        EXPECT_EQ(context.visual_context_tree_copy_count_for_testing(), update);
    }
}

TEST_CASE(oversized_backing_stores_are_rejected)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed({ 40'000, 40'000 }, Compositing::WindowResizingInProgress::No, true);
    VERIFY(allocation.has_value());

    auto publication = manager.allocate_backing_stores(*allocation, {}, true, Compositor::BackingStoreManager::GpuSharing::Disallowed);

    EXPECT(!publication.has_value());
    EXPECT(!manager.is_valid());
}

#ifdef AK_OS_MACOS
TEST_CASE(a_released_backing_store_is_not_reused_while_its_surface_is_in_use)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed({ 4, 4 }, Compositing::WindowResizingInProgress::No, true);
    VERIFY(allocation.has_value());
    auto publication = manager.allocate_backing_stores(*allocation, {}, true, Compositor::BackingStoreManager::GpuSharing::Disallowed);
    VERIFY(publication.has_value());
    EXPECT_EQ(publication->bitmap_ids.size(), 3u);

    auto imported_backing_stores = import_shared_images(publication->shared_images);
    Vector<Core::IOSurfaceHandle> surfaces_as_seen_by_the_presenting_process;
    for (auto const& shared_image_buffer : imported_backing_stores)
        surfaces_as_seen_by_the_presenting_process.append(handle_for_marking_in_use(shared_image_buffer));
    for (auto const& surface : surfaces_as_seen_by_the_presenting_process)
        EXPECT(!surface.is_in_use());

    auto present_into = [&](size_t store_index) {
        auto render_target = manager.acquire_render_target({});
        VERIFY(render_target.has_value());
        EXPECT_EQ(render_target->bitmap_id, publication->bitmap_ids[store_index]);
        manager.complete_rendering(publication->bitmap_ids[store_index], true);
    };

    // The first store is reserved as the initial front buffer. Present each store once, as only a store the client
    // has been presented can be read by the window server.
    present_into(1);
    VERIFY(manager.release_buffer(publication->bitmap_ids[0]));
    present_into(0);
    VERIFY(manager.release_buffer(publication->bitmap_ids[1]));
    surfaces_as_seen_by_the_presenting_process[1].increment_use_count();
    present_into(2);
    VERIFY(manager.release_buffer(publication->bitmap_ids[0]));

    // The client shows the third store; the window server keeps reading the other two.
    surfaces_as_seen_by_the_presenting_process[0].increment_use_count();
    EXPECT(!manager.has_available_buffer());
    EXPECT(!manager.acquire_render_target({}).has_value());

    surfaces_as_seen_by_the_presenting_process[1].decrement_use_count();
    EXPECT(manager.has_available_buffer());
    present_into(1);
    EXPECT(!manager.has_available_buffer());

    VERIFY(manager.release_buffer(publication->bitmap_ids[2]));
    EXPECT(manager.has_available_buffer());
    surfaces_as_seen_by_the_presenting_process[2].increment_use_count();
    EXPECT(!manager.has_available_buffer());
    surfaces_as_seen_by_the_presenting_process[2].decrement_use_count();
    surfaces_as_seen_by_the_presenting_process[0].decrement_use_count();
    present_into(0);
}

TEST_CASE(a_backing_store_is_added_only_when_the_window_server_reads_every_released_one_and_starts_fully_damaged)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed({ 4, 4 }, Compositing::WindowResizingInProgress::No, true);
    VERIFY(allocation.has_value());
    auto publication = manager.allocate_backing_stores(*allocation, {}, true, Compositor::BackingStoreManager::GpuSharing::Disallowed);
    VERIFY(publication.has_value());

    auto imported_backing_stores = import_shared_images(publication->shared_images);
    Vector<Core::IOSurfaceHandle> surfaces_as_seen_by_the_presenting_process;
    for (auto const& shared_image_buffer : imported_backing_stores)
        surfaces_as_seen_by_the_presenting_process.append(handle_for_marking_in_use(shared_image_buffer));

    auto present_into = [&](size_t store_index) {
        auto render_target = manager.acquire_render_target({});
        VERIFY(render_target.has_value());
        EXPECT_EQ(render_target->bitmap_id, publication->bitmap_ids[store_index]);
        manager.complete_rendering(publication->bitmap_ids[store_index], true);
    };

    // Nothing was presented yet, so there is no frame on screen for the window server to be holding stores behind.
    EXPECT(!manager.add_backing_store_if_window_server_still_reads_every_released_store({}).has_value());

    present_into(1);
    VERIFY(manager.release_buffer(publication->bitmap_ids[0]));
    present_into(0);
    surfaces_as_seen_by_the_presenting_process[1].increment_use_count();
    surfaces_as_seen_by_the_presenting_process[0].increment_use_count();

    // The client has yet to release the store it showed before this one.
    EXPECT(!manager.add_backing_store_if_window_server_still_reads_every_released_store({}).has_value());
    VERIFY(manager.release_buffer(publication->bitmap_ids[1]));

    // The third store can still be rendered into.
    EXPECT(!manager.add_backing_store_if_window_server_still_reads_every_released_store({}).has_value());
    present_into(2);
    VERIFY(manager.release_buffer(publication->bitmap_ids[0]));
    EXPECT(!manager.has_available_buffer());

    auto added_publication = manager.add_backing_store_if_window_server_still_reads_every_released_store({});
    VERIFY(added_publication.has_value());
    EXPECT_EQ(added_publication->bitmap_ids.size(), 1u);
    EXPECT_EQ(added_publication->shared_images.size(), 1u);
    EXPECT(!publication->bitmap_ids.contains_slow(added_publication->bitmap_ids[0]));
    EXPECT(manager.has_surplus_backing_stores());

    auto render_target = manager.acquire_render_target({});
    VERIFY(render_target.has_value());
    EXPECT_EQ(render_target->bitmap_id, added_publication->bitmap_ids[0]);
    EXPECT_EQ(render_target->damage_rect, (Gfx::IntRect { 0, 0, 4, 4 }));
    manager.complete_rendering(added_publication->bitmap_ids[0], true);

    surfaces_as_seen_by_the_presenting_process[0].decrement_use_count();
    surfaces_as_seen_by_the_presenting_process[1].decrement_use_count();
}

TEST_CASE(a_backing_store_the_client_was_never_presented_is_rendered_into_while_its_send_right_is_in_flight)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed({ 4, 4 }, Compositing::WindowResizingInProgress::No, true);
    VERIFY(allocation.has_value());
    // The publication is not imported, so its send rights keep every surface reading as in use.
    auto publication = manager.allocate_backing_stores(*allocation, {}, true, Compositor::BackingStoreManager::GpuSharing::Disallowed);
    VERIFY(publication.has_value());

    EXPECT(manager.has_available_buffer());
    auto render_target = manager.acquire_render_target({});
    VERIFY(render_target.has_value());
    EXPECT_EQ(render_target->bitmap_id, publication->bitmap_ids[1]);
    manager.complete_rendering(publication->bitmap_ids[1], true);

    // Once the client has been presented the store, use of its surface keeps the compositor out of it.
    VERIFY(manager.release_buffer(publication->bitmap_ids[1]));
    auto render_target_skipping_the_presented_store = manager.acquire_render_target({});
    VERIFY(render_target_skipping_the_presented_store.has_value());
    EXPECT_EQ(render_target_skipping_the_presented_store->bitmap_id, publication->bitmap_ids[2]);
    manager.complete_rendering(publication->bitmap_ids[2], true);
}
#endif

TEST_CASE(viewport_scrollbar_collapses_when_drag_is_released_away_from_scrollbar)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    auto hover_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 10));
    EXPECT(hover_result.accepted);
    EXPECT(hover_result.frame_to_present.has_value());

    auto press_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary));
    EXPECT(press_result.accepted);

    auto drag_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 50, 50));
    EXPECT(drag_result.accepted);

    auto release_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 50, 50, Web::UIEvents::MouseButton::Primary));
    EXPECT(release_result.accepted);

    // Releasing the drag should have already cleared the hover state, so the next move must not trigger a delayed repaint.
    auto next_move_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 50, 50));
    EXPECT(!next_move_result.accepted);
    EXPECT(!next_move_result.frame_to_present.has_value());
}

TEST_CASE(viewport_scrollbar_drag_ignores_non_primary_mouse_up)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 10)).accepted);
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary)).accepted);

    auto secondary_release_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 50, 50, Web::UIEvents::MouseButton::Secondary));
    EXPECT(!secondary_release_result.accepted);

    // The primary-button drag remains captured after another button is released.
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 50, 60)).accepted);
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 50, 60, Web::UIEvents::MouseButton::Primary)).accepted);
}

// MonotonicTime has no fixed reference point, so the pacing tests offset from one taken once.
static MonotonicTime monotonic_time_at(i64 nanoseconds)
{
    static auto const reference = MonotonicTime::now();
    return reference + AK::Duration::from_nanoseconds(nanoseconds);
}

static size_t count_frames_paced_over_one_second(Compositor::FramePacer& pacer, double display_refresh_rate, i64 jitter_nanoseconds = 0)
{
    size_t frames = 0;
    auto ticks = static_cast<i64>(display_refresh_rate);
    for (i64 tick = 0; tick < ticks; ++tick) {
        // Real display ticks wobble around their period; alternate early and late ticks by the jitter.
        auto jitter = tick % 2 ? jitter_nanoseconds : -jitter_nanoseconds;
        auto frame_time = monotonic_time_at(1'000'000'000 + tick * 1'000'000'000 / ticks + jitter);
        if (!pacer.is_due(frame_time, display_refresh_rate))
            continue;
        pacer.did_deliver(frame_time);
        ++frames;
    }
    return frames;
}

TEST_CASE(a_frame_pacer_rounds_its_interval_up_to_whole_display_ticks)
{
    Compositor::FramePacer pacer;
    EXPECT_APPROXIMATE(pacer.frame_interval(60), 1000.0 / 60);

    pacer.set_maximum_frames_per_second(30);
    EXPECT_APPROXIMATE(pacer.frame_interval(60), 2000.0 / 60);
    EXPECT_APPROXIMATE(pacer.frame_interval(120), 4000.0 / 120);

    // A rate between two whole tick counts rounds down to the slower one.
    pacer.set_maximum_frames_per_second(45);
    EXPECT_APPROXIMATE(pacer.frame_interval(60), 2000.0 / 60);

    // A rate above the display's delivers on every tick, never more often.
    pacer.set_maximum_frames_per_second(120);
    EXPECT_APPROXIMATE(pacer.frame_interval(60), 1000.0 / 60);
    EXPECT_APPROXIMATE(pacer.frame_interval(120), 1000.0 / 120);
}

TEST_CASE(a_frame_pacer_delivers_its_rate_over_display_ticks)
{
    struct Case {
        double maximum_frames_per_second;
        double display_refresh_rate;
        size_t expected_frames;
    };
    for (auto [maximum_frames_per_second, display_refresh_rate, expected_frames] : Array {
             Case { 30, 60, 30 },
             Case { 60, 60, 60 },
             Case { 120, 60, 60 },
             Case { 30, 120, 30 },
             Case { 60, 120, 60 },
             Case { 120, 120, 120 },
         }) {
        for (i64 jitter_nanoseconds : { 0, 1'000'000 }) {
            Compositor::FramePacer pacer;
            pacer.set_maximum_frames_per_second(maximum_frames_per_second);
            EXPECT_EQ(count_frames_paced_over_one_second(pacer, display_refresh_rate, jitter_nanoseconds), expected_frames);
        }
    }
}

TEST_CASE(a_frame_pacer_delivers_its_first_frame_on_any_tick)
{
    Compositor::FramePacer pacer;
    pacer.set_maximum_frames_per_second(1);
    EXPECT(pacer.is_due(monotonic_time_at(0), 60));
    pacer.did_deliver(monotonic_time_at(0));
    EXPECT(!pacer.is_due(monotonic_time_at(500'000'000), 60));
    EXPECT(pacer.is_due(monotonic_time_at(1'000'000'000), 60));
}

TEST_CASE(requesting_a_rendering_opportunity_again_only_updates_its_rate)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 1 }, 1, client, canvas_surface_registry };

    EXPECT(context.request_rendering_opportunity(60));
    EXPECT(!context.request_rendering_opportunity(30));
    EXPECT(context.rendering_opportunity_requested());
    EXPECT_APPROXIMATE(context.rendering_opportunity_frame_interval(60), 2000.0 / 60);

    auto frame_time = monotonic_time_at(1'000'000'000);
    EXPECT(context.rendering_opportunity_is_due(frame_time, 60));
    context.did_deliver_rendering_opportunity(frame_time);
    EXPECT(!context.rendering_opportunity_requested());
    EXPECT(!context.rendering_opportunity_is_due(frame_time + AK::Duration::from_milliseconds(17), 60));
    EXPECT(context.rendering_opportunity_is_due(frame_time + AK::Duration::from_milliseconds(33), 60));
}

TEST_CASE(hidden_context_coalesces_presents_and_presents_once_when_shown)
{
    Core::EventLoop event_loop;
    TestCompositorClient compositor_client;
    TestWebContentClient web_content_client;
    auto compositor_state = Compositor::CompositorState::create({});
    compositor_state->set_client(compositor_client);

    u64 page_id = 1;
    auto context_id = Web::compositor_context_id_for_page(page_id);
    auto viewport_rect = Gfx::IntRect { 0, 0, 4, 4 };
    auto visual_context_tree = Compositing::VisualContextTreeTestBuilder().finish();

    compositor_state->create_context(context_id, page_id, web_content_client);
    compositor_state->viewport_size_updated(context_id, viewport_rect.size(), Compositing::WindowResizingInProgress::No);
    EXPECT(!compositor_client.allocated_bitmap_ids.is_empty());
    compositor_state->update_display_list(context_id, make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree, {}, {});

    compositor_state->set_context_visibility(context_id, Compositing::ContextVisibility::Hidden);
    compositor_state->present_frame(context_id, viewport_rect);
    compositor_state->present_frame(context_id, viewport_rect);
    compositor_state->presented_bitmap_ready_to_paint(context_id, compositor_client.allocated_bitmap_ids[0]);
    EXPECT(!spin_event_loop_until(event_loop, 100, [&] { return !compositor_client.presented_frames.is_empty(); }));

    compositor_state->set_context_visibility(context_id, Compositing::ContextVisibility::Visible);
    EXPECT(spin_event_loop_until(event_loop, 2000, [&] { return !compositor_client.presented_frames.is_empty(); }));
    EXPECT(!spin_event_loop_until(event_loop, 100, [&] { return compositor_client.presented_frames.size() > 1; }));
    EXPECT_EQ(compositor_client.presented_frames.size(), 1u);
    EXPECT_EQ(compositor_client.presented_frames[0].damage_rect, viewport_rect);
}

TEST_CASE(dragging_a_viewport_scrollbar_reports_a_user_scroll_gesture_until_it_is_released)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary)).accepted);
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT(updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 50)).accepted);
    updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.scroll_offsets.is_empty());
    EXPECT(updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);

    // The release always asks for a rendering update, so that the main thread learns of it even when nothing scrolled.
    auto release_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 98, 50, Web::UIEvents::MouseButton::Primary));
    EXPECT(release_result.accepted);
    EXPECT(release_result.should_request_rendering_update);
    updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(updates.user_scroll_gesture_ended);

    // The release is reported once.
    updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);
}

struct RecordingWebContentClient final : public Compositor::CompositorStateWebContentClient {
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const&) override { }
    virtual void dispatch_key_event_to_web_content(u64, Web::KeyEvent const&) override { }
    virtual void request_rendering_update() override { events.append("request_rendering_update"_string); }
    virtual void rendering_opportunity(Web::CompositorContextId, i64, double) override { }
    virtual void clock_tick(Web::CompositorContextId, i64, double, Vector<Web::CompositorScrollOffset> const&) override { }
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates const& updates) override
    {
        events.append("async_scroll_updates"_string);
        pushed_updates.append(updates);
    }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool) override { }

    String event_sequence() const { return MUST(String::join(","sv, events)); }

    Vector<String> events;
    Vector<Compositing::PendingAsyncScrollUpdates> pushed_updates;
};

TEST_CASE(pending_scroll_updates_go_ahead_of_a_rendering_update_request)
{
    RecordingWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    // A rendering update run on the request must find what the compositor had pending at the time, so the pending
    // updates go out first.
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary)).accepted);
    context.request_rendering_update();
    EXPECT_EQ(client.event_sequence(), "async_scroll_updates,request_rendering_update"sv);
    EXPECT_EQ(client.pushed_updates.size(), 1u);
    EXPECT(client.pushed_updates.last().user_scroll_gesture_in_progress);
    EXPECT(!context.has_pending_async_scroll_updates());

    // Nothing pending, nothing pushed.
    context.request_rendering_update();
    EXPECT_EQ(client.event_sequence(), "async_scroll_updates,request_rendering_update,request_rendering_update"sv);
}

TEST_CASE(dragging_a_scrollbar_thumb_scrolls_its_scroller_to_where_the_thumb_was_dragged)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    // The thumb is 20 device pixels long and travels 0.8 device pixels per scrolled pixel.
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary)).accepted);
    EXPECT(context.take_pending_async_scroll_updates().scroll_offsets.is_empty());

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 50)).accepted);
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets[0].compositor_scroll_offset, (Gfx::FloatPoint { 0, 50 }));
    EXPECT_EQ(updates.scroll_offsets[0].unadopted_scroll_delta, (Gfx::FloatPoint { 0, 50 }));
    EXPECT_EQ(updates.scroll_offsets[0].last_relative_scroll_delta, (Gfx::FloatPoint {}));

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 500)).accepted);
    updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets[0].compositor_scroll_offset, (Gfx::FloatPoint { 0, 100 }));
    EXPECT_EQ(updates.scroll_offsets[0].unadopted_scroll_delta, (Gfx::FloatPoint { 0, 50 }));

    // Dragging further past the end scrolls nothing.
    auto result_past_the_end = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 900));
    EXPECT(result_past_the_end.accepted);
    EXPECT(!result_past_the_end.frame_to_present.has_value());
    EXPECT(context.take_pending_async_scroll_updates().scroll_offsets.is_empty());
}

TEST_CASE(a_render_clock_frame_carries_where_the_compositor_has_scrolled_to)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    // A render clock tick samples scroll-driven animations where the page is scrolled to, before the main thread has
    // taken the scroll in.
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary)).accepted);
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 50)).accepted);
    auto scroll_offsets = context.scroll_offsets();
    EXPECT_EQ(scroll_offsets.size(), 1u);
    EXPECT_EQ(scroll_offsets[0].scroll_node.kind, Web::AsyncScrollNodeKind::Viewport);
    EXPECT_EQ(scroll_offsets[0].offset, Web::CSSPixelPoint(0, 50));
}

TEST_CASE(losing_the_scrollbar_a_drag_holds_ends_its_user_scroll_gesture)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary)).accepted);
    EXPECT(context.take_pending_async_scroll_updates().user_scroll_gesture_in_progress);

    // The drag cannot outlive the scrollbar it holds.
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree, false), visual_context_tree, {});
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(updates.user_scroll_gesture_ended);
}
TEST_CASE(ui_overlay_uses_the_current_viewport_size)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };

    context.viewport_size_updated({ 640, 480 }, Compositing::WindowResizingInProgress::No);
    context.did_submit_prepared_frame({ 12, 18, 640, 480 });

    context.viewport_size_updated({ 800, 600 }, Compositing::WindowResizingInProgress::Yes);
    EXPECT_EQ(context.viewport_rect_for_ui_overlay(), (Gfx::IntRect { 12, 18, 800, 600 }));

    context.queue_present_frame({ { 30, 40, 800, 600 }, { 0, 0, 800, 600 } });
    context.viewport_size_updated({ 1024, 768 }, Compositing::WindowResizingInProgress::Yes);
    EXPECT_EQ(context.viewport_rect_for_ui_overlay(), (Gfx::IntRect { 30, 40, 1024, 768 }));
}

TEST_CASE(ui_overlay_hover_changes_require_repainting)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };

    EXPECT(context.set_paused_debugger_overlay(true, 1.0, {}, {}));
    EXPECT(!context.set_paused_debugger_overlay(true, 1.0, {}, {}));
    EXPECT(context.set_paused_debugger_overlay(true, 1.0, {}, Compositing::PausedDebuggerOverlayAction::StepOver));
    EXPECT(!context.set_paused_debugger_overlay(true, 1.0, {}, Compositing::PausedDebuggerOverlayAction::StepOver));
    EXPECT(context.set_paused_debugger_overlay(true, 1.0, {}, {}));
}

struct Fill {
    Gfx::IntRect rect;
    Gfx::Color color;
    Compositing::ContextRef context {};
    bool bounded { true };
};

static NonnullRefPtr<Compositing::DisplayList> make_fills_display_list(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Vector<Fill> const& fills, Optional<Gfx::Color> surface_clear_color = {}, Optional<Compositing::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata = {})
{
    TestDisplayList command_bytes;
    for (auto const& fill : fills) {
        Compositing::FillRect command { fill.rect, fill.color, Gfx::CompositingAndBlendingOperator::Normal, Compositing::NO_EFFECT_NODE };
        append_display_list_command(command_bytes, command, fill.bounded ? Optional<Gfx::IntRect> { fill.rect } : Optional<Gfx::IntRect> {}, fill.context);
    }
    return decode_display_list(visual_context_tree, move(command_bytes), surface_clear_color, async_scrolling_metadata);
}

static Compositing::AccumulatedVisualContextTree make_translated_visual_context_tree(Gfx::FloatPoint translation, Optional<u64> structural_epoch = {})
{
    Compositing::VisualContextTreeTestBuilder builder;
    builder.append_transform(Compositing::VISUAL_VIEWPORT_NODE_INDEX, Gfx::translation_matrix(Gfx::FloatVector3 { translation.x(), translation.y(), 0 }));
    if (structural_epoch.has_value())
        return builder.finish_with_structural_epoch(*structural_epoch);
    return builder.finish();
}

static Compositing::ScrollStateSnapshot scroll_state_snapshot_with_offset(Compositing::SpatialNodeIndex index, Gfx::FloatPoint device_offset)
{
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    scroll_state_snapshot.set_device_offset_for_index(index, device_offset);
    return scroll_state_snapshot;
}

static constexpr Compositing::ContextRef in_spatial_node(u32 index)
{
    return { Compositing::SpatialNodeIndex { index } };
}

enum class TargetCoveringNestedScrollbar {
    None,
    PaintedBeforeScrollbar,
    PaintedAfterScrollbar,
};

struct NestedScrollbarSceneOptions {
    Gfx::FloatPoint translation_of_scroller {};
    Optional<Gfx::FloatRect> clip_of_scroller {};
    TargetCoveringNestedScrollbar target_covering_scrollbar { TargetCoveringNestedScrollbar::None };
    bool display_list_paints_enlarged_scrollbar { false };
    bool gives_nested_scroller_a_later_scroll_node_index { false };
    Optional<Gfx::FloatRect> main_thread_wheel_event_region_in_viewport {};
};

struct NestedScrollbarScene {
    Compositing::AccumulatedVisualContextTree visual_context_tree;
    NonnullRefPtr<Compositing::DisplayList> display_list;
    Compositing::SpatialNodeIndex viewport_scroll_node_index;
    Compositing::SpatialNodeIndex nested_scroll_node_index;
};

static Web::UniqueNodeID const viewport_scroller_node_id { 2 };
static Web::UniqueNodeID const nested_scroller_node_id { 3 };

// A viewport that scrolls by 100 holds a 40x40 scroller at 10,10 that scrolls by 120. The scroller's vertical scrollbar
// is painted by the display list: its track is 46,10 4x40, enlarged 42,10 8x40, and its 10 long thumb travels 0.25 device
// pixels per scrolled pixel.
static NestedScrollbarScene make_nested_scrollbar_scene(NestedScrollbarSceneOptions options = {})
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node_index = builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    if (options.gives_nested_scroller_a_later_scroll_node_index)
        builder.append_scroll(viewport_scroll_node_index);
    auto spatial_node_of_scroller = viewport_scroll_node_index;
    if (!options.translation_of_scroller.is_zero())
        spatial_node_of_scroller = builder.append_transform(viewport_scroll_node_index, Gfx::translation_matrix(Gfx::FloatVector3 { options.translation_of_scroller.x(), options.translation_of_scroller.y(), 0 }));
    Compositing::ContextRef context_of_scroller { spatial_node_of_scroller };
    if (options.clip_of_scroller.has_value())
        context_of_scroller.clip = builder.append_clip(Compositing::NO_CLIP_NODE, spatial_node_of_scroller, *options.clip_of_scroller);
    auto nested_scroll_node_index = builder.append_scroll(spatial_node_of_scroller);
    auto visual_context_tree = builder.finish();

    Web::UniqueNodeID document_id { 1 };
    TestDisplayList command_bytes;
    append_display_list_command(
        command_bytes,
        Compositing::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = viewport_scroller_node_id,
            .scroll_node_index = viewport_scroll_node_index,
            .parent_scroll_node_index = Compositing::VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Compositing::CompositorScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
            .is_viewport = true,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        });
    if (options.main_thread_wheel_event_region_in_viewport.has_value()) {
        append_display_list_command(
            command_bytes,
            Compositing::CompositorMainThreadWheelEventRegion { .rect = *options.main_thread_wheel_event_region_in_viewport },
            {},
            in_spatial_node(viewport_scroll_node_index.value()));
    }
    append_display_list_command(
        command_bytes,
        Compositing::CompositorWheelHitTestTarget {
            .document_id = document_id,
            .target_scroll_node_index = nested_scroll_node_index,
            .rect = { 10, 10, 40, 40 },
        },
        {},
        context_of_scroller);
    append_display_list_command(
        command_bytes,
        Compositing::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = nested_scroller_node_id,
            .scroll_node_index = nested_scroll_node_index,
            .parent_scroll_node_index = viewport_scroll_node_index,
            .scrollport_rect = { 10, 10, 40, 40 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 120 },
            .scroll_node_kind = Compositing::CompositorScrollNodeKind::Element,
            .pseudo_element_type = 0,
            .is_viewport = false,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        },
        {},
        context_of_scroller);
    append_display_list_command(
        command_bytes,
        Compositing::CompositorWheelHitTestTarget {
            .document_id = document_id,
            .target_scroll_node_index = nested_scroll_node_index,
            .rect = { 10, 10, 40, 140 },
        },
        {},
        in_spatial_node(nested_scroll_node_index.value()));

    auto append_target_covering_scrollbar = [&] {
        append_display_list_command(
            command_bytes,
            Compositing::CompositorWheelHitTestTarget {
                .document_id = document_id,
                .target_scroll_node_index = viewport_scroll_node_index,
                .rect = { 40, 0, 30, 30 },
            },
            {},
            in_spatial_node(viewport_scroll_node_index.value()));
    };
    if (options.target_covering_scrollbar == TargetCoveringNestedScrollbar::PaintedBeforeScrollbar)
        append_target_covering_scrollbar();
    append_display_list_command(
        command_bytes,
        Compositing::CompositorScrollbar {
            .document_id = document_id,
            .scroll_node_index = nested_scroll_node_index,
            .gutter_rect = {},
            .thumb_rect = { 47, 10, 2, 10 },
            .track_rect = { 46, 10, 4, 40 },
            .expanded_gutter_rect = { 42, 10, 8, 40 },
            .expanded_thumb_rect = { 44, 10, 4, 10 },
            .scroll_size = 0.25,
            .expanded_scroll_size = 0.25,
            .min_scroll_offset = 0,
            .max_scroll_offset = 120,
            .thumb_color = Gfx::Color::Black,
            .track_color = Gfx::Color::Transparent,
            .vertical = true,
            .is_painted_by_compositor = false,
            .display_list_paints_enlarged_scrollbar = options.display_list_paints_enlarged_scrollbar,
        },
        {},
        context_of_scroller);
    if (options.target_covering_scrollbar == TargetCoveringNestedScrollbar::PaintedAfterScrollbar)
        append_target_covering_scrollbar();

    auto display_list = decode_display_list(visual_context_tree, move(command_bytes), {},
        Compositing::DisplayList::AsyncScrollingMetadata {
            .viewport_rect = { 0, 0, 100, 100 },
        });
    return { move(visual_context_tree), move(display_list), viewport_scroll_node_index, nested_scroll_node_index };
}

struct NestedScrollbarContextFixture {
    explicit NestedScrollbarContextFixture(NestedScrollbarSceneOptions options = {}, Optional<Gfx::FloatPoint> device_offset_of_viewport = {})
    {
        install(options, device_offset_of_viewport);
    }

    void install(NestedScrollbarSceneOptions options = {}, Optional<Gfx::FloatPoint> device_offset_of_viewport = {})
    {
        auto scene = make_nested_scrollbar_scene(options);
        Compositing::ScrollStateSnapshot scroll_state_snapshot;
        if (device_offset_of_viewport.has_value())
            scroll_state_snapshot = scroll_state_snapshot_with_offset(scene.viewport_scroll_node_index, *device_offset_of_viewport);
        context.install_display_list_update(scene.display_list, scene.visual_context_tree, move(scroll_state_snapshot));
    }

    bool press_is_taken_at(int x, int y)
    {
        auto taken = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, x, y, Web::UIEvents::MouseButton::Primary)).accepted;
        if (taken)
            context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, x, y, Web::UIEvents::MouseButton::Primary));
        context.take_pending_async_scroll_updates();
        return taken;
    }

    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
};

TEST_CASE(dragging_a_nested_scrollbar_scrolls_its_scroller_and_names_it_for_the_main_thread)
{
    NestedScrollbarContextFixture fixture;
    auto& context = fixture.context;

    EXPECT(!context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 15)).accepted);

    auto press_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 48, 15, Web::UIEvents::MouseButton::Primary));
    EXPECT(press_result.accepted);
    EXPECT(press_result.scrollbar_dragged_by_compositor.has_value());
    EXPECT_EQ(press_result.scrollbar_dragged_by_compositor->scroller_stable_node_id.node_id, nested_scroller_node_id);
    EXPECT_EQ(press_result.scrollbar_dragged_by_compositor->scroller_stable_node_id.kind, Web::AsyncScrollNodeKind::Element);
    EXPECT(press_result.scrollbar_dragged_by_compositor->vertical);
    // The display list paints this scrollbar, so nothing has to be repainted for a press that scrolls nothing.
    EXPECT(!press_result.frame_to_present.has_value());
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT(updates.scroll_offsets.is_empty());
    EXPECT(updates.user_scroll_gesture_in_progress);

    auto drag_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 30));
    EXPECT(drag_result.accepted);
    EXPECT(drag_result.scrollbar_dragged_by_compositor.has_value());
    EXPECT(drag_result.frame_to_present.has_value());
    EXPECT_EQ(drag_result.frame_to_present->viewport_rect, (Gfx::IntRect { 0, 0, 100, 100 }));
    EXPECT(drag_result.frame_to_present->forced_damage_rect.is_empty());
    updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets[0].stable_node_id.node_id, nested_scroller_node_id);
    EXPECT_EQ(updates.scroll_offsets[0].compositor_scroll_offset, (Gfx::FloatPoint { 0, 60 }));

    auto release_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 48, 30, Web::UIEvents::MouseButton::Primary));
    EXPECT(release_result.accepted);
    EXPECT(release_result.scrollbar_dragged_by_compositor.has_value());
    EXPECT(release_result.should_request_rendering_update);
    updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(updates.user_scroll_gesture_ended);

    EXPECT(!context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 30)).accepted);
}

TEST_CASE(a_viewport_scrollbar_drag_is_not_named_for_the_main_thread)
{
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree, {});

    auto press_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary));
    EXPECT(press_result.accepted);
    EXPECT(!press_result.scrollbar_dragged_by_compositor.has_value());
    EXPECT(!context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 50)).scrollbar_dragged_by_compositor.has_value());
}

TEST_CASE(dragging_a_nested_scrollbar_past_its_end_does_not_scroll_the_viewport)
{
    NestedScrollbarContextFixture fixture;
    auto& context = fixture.context;

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 48, 15, Web::UIEvents::MouseButton::Primary)).accepted);
    auto drag_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 500));
    EXPECT(drag_result.frame_to_present.has_value());
    EXPECT_EQ(drag_result.frame_to_present->viewport_rect, (Gfx::IntRect { 0, 0, 100, 100 }));
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets[0].stable_node_id.node_id, nested_scroller_node_id);
    EXPECT_EQ(updates.scroll_offsets[0].compositor_scroll_offset, (Gfx::FloatPoint { 0, 120 }));

    // The scroller is at its end and the viewport could still scroll, but a scrollbar only scrolls its own scroller.
    auto result_past_the_end = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 900));
    EXPECT(result_past_the_end.accepted);
    EXPECT(!result_past_the_end.frame_to_present.has_value());
    EXPECT(context.take_pending_async_scroll_updates().scroll_offsets.is_empty());
}

TEST_CASE(a_nested_scrollbar_is_hit_where_its_scrolled_ancestor_puts_it)
{
    NestedScrollbarContextFixture fixture { {}, Gfx::FloatPoint { 0, -20 } };
    // The viewport is scrolled by 20, so the track spans -10 to 30 on screen.
    EXPECT(!fixture.press_is_taken_at(48, 45));
    EXPECT(fixture.press_is_taken_at(48, 5));
}

TEST_CASE(a_nested_scrollbar_is_hit_where_a_transform_puts_it)
{
    NestedScrollbarContextFixture fixture { { .translation_of_scroller = { 30, 0 } } };
    EXPECT(!fixture.press_is_taken_at(48, 15));
    EXPECT(fixture.press_is_taken_at(78, 15));
}

TEST_CASE(a_nested_scrollbar_is_not_hit_where_it_is_clipped_away_but_its_drag_continues_there)
{
    NestedScrollbarContextFixture fixture { { .clip_of_scroller = Gfx::FloatRect { 0, 0, 100, 25 } } };
    auto& context = fixture.context;
    EXPECT(!fixture.press_is_taken_at(48, 30));

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 48, 15, Web::UIEvents::MouseButton::Primary)).accepted);
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 30)).accepted);
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets[0].compositor_scroll_offset, (Gfx::FloatPoint { 0, 60 }));
}

TEST_CASE(a_nested_scrollbar_is_not_hit_under_something_painted_over_it)
{
    NestedScrollbarContextFixture covered_after { { .target_covering_scrollbar = TargetCoveringNestedScrollbar::PaintedAfterScrollbar } };
    // The covering target spans 40,0 30x30.
    EXPECT(!covered_after.press_is_taken_at(48, 15));
    EXPECT(covered_after.press_is_taken_at(48, 40));

    NestedScrollbarContextFixture covered_before { { .target_covering_scrollbar = TargetCoveringNestedScrollbar::PaintedBeforeScrollbar } };
    EXPECT(covered_before.press_is_taken_at(48, 15));
}

TEST_CASE(a_nested_scrollbar_is_hit_within_the_rect_the_display_list_paints_it_in)
{
    NestedScrollbarContextFixture regular;
    EXPECT(!regular.press_is_taken_at(43, 15));
    EXPECT(regular.press_is_taken_at(47, 15));

    NestedScrollbarContextFixture enlarged { { .display_list_paints_enlarged_scrollbar = true } };
    EXPECT(enlarged.press_is_taken_at(43, 15));
}

TEST_CASE(a_nested_scrollbar_drag_outlives_a_display_list_that_renumbers_its_scroll_node)
{
    NestedScrollbarContextFixture fixture;
    auto& context = fixture.context;

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 48, 15, Web::UIEvents::MouseButton::Primary)).accepted);
    context.take_pending_async_scroll_updates();

    fixture.install({ .display_list_paints_enlarged_scrollbar = true, .gives_nested_scroller_a_later_scroll_node_index = true });
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT(updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);

    auto drag_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 30));
    EXPECT(drag_result.accepted);
    EXPECT(drag_result.scrollbar_dragged_by_compositor.has_value());
    updates = context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets[0].stable_node_id.node_id, nested_scroller_node_id);
    EXPECT_EQ(updates.scroll_offsets[0].compositor_scroll_offset, (Gfx::FloatPoint { 0, 60 }));
}

TEST_CASE(losing_the_nested_scrollbar_a_drag_holds_ends_its_user_scroll_gesture)
{
    NestedScrollbarContextFixture fixture;
    auto& context = fixture.context;

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 48, 15, Web::UIEvents::MouseButton::Primary)).accepted);
    EXPECT(context.take_pending_async_scroll_updates().user_scroll_gesture_in_progress);

    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree, false), visual_context_tree, {});
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(updates.user_scroll_gesture_ended);
    EXPECT(!context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 48, 30)).accepted);
}

// Drives wheel events from the UI at chosen times through the nested scrollbar scene. (20,20) is over the nested
// scroller, (80,80) over the viewport.
struct LatchedWheelContextFixture {
    explicit LatchedWheelContextFixture(NestedScrollbarSceneOptions options = {})
        : scene(options)
    {
        scene.context.viewport_size_updated({ 100, 100 }, Compositing::WindowResizingInProgress::No);
    }

    Compositor::ContextState::ContextUpdateResult wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::ScrollGesturePhase phase, i64 milliseconds_after_start, u32 modifiers = Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision precision = Web::WheelDeltaPrecision::Precise)
    {
        return scene.context.async_scroll_by(position, delta, precision, phase, modifiers, now + AK::Duration::from_milliseconds(milliseconds_after_start));
    }

    struct TakenScrollOffsets {
        Optional<Gfx::FloatPoint> viewport;
        Optional<Gfx::FloatPoint> nested;
    };

    TakenScrollOffsets take_scroll_offsets()
    {
        TakenScrollOffsets taken;
        for (auto const& scroll_offset : scene.context.take_pending_async_scroll_updates().scroll_offsets) {
            if (scroll_offset.stable_node_id.node_id == viewport_scroller_node_id)
                taken.viewport = scroll_offset.compositor_scroll_offset;
            if (scroll_offset.stable_node_id.node_id == nested_scroller_node_id)
                taken.nested = scroll_offset.compositor_scroll_offset;
        }
        return taken;
    }

    Optional<Web::UniqueNodeID> latched_scroller_node_id() const
    {
        return scene.context.latched_wheel_scroller_for_testing().map([](auto const& stable_node_id) { return stable_node_id.node_id; });
    }

    // A gesture whose first step takes the nested scroller to its edge, so that a step of it is absorbed there, while
    // a step routed afresh goes past the scroller to the viewport.
    void latch_gesture_to_nested_scroller_at_its_edge(Web::ScrollGesturePhase phase = Web::ScrollGesturePhase::Ongoing, Web::WheelDeltaPrecision precision = Web::WheelDeltaPrecision::Precise)
    {
        EXPECT(wheel({ 20, 20 }, { 0, 120 }, phase, 0, Web::UIEvents::KeyModifier::Mod_None, precision).accepted);
        EXPECT_EQ(take_scroll_offsets().nested, (Gfx::FloatPoint { 0, 120 }));
    }

    void expect_step_to_be_absorbed_by_latched_scroller(Gfx::FloatPoint position, Web::ScrollGesturePhase phase, i64 milliseconds_after_start, u32 modifiers = Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision precision = Web::WheelDeltaPrecision::Precise)
    {
        EXPECT(wheel(position, { 0, 50 }, phase, milliseconds_after_start, modifiers, precision).accepted);
        auto offsets = take_scroll_offsets();
        EXPECT(!offsets.nested.has_value());
        EXPECT(!offsets.viewport.has_value());
    }

    void expect_step_to_scroll_viewport_afresh(Gfx::FloatPoint position, Web::ScrollGesturePhase phase, i64 milliseconds_after_start, u32 modifiers = Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision precision = Web::WheelDeltaPrecision::Precise)
    {
        EXPECT(wheel(position, { 0, 50 }, phase, milliseconds_after_start, modifiers, precision).accepted);
        EXPECT_EQ(take_scroll_offsets().viewport, (Gfx::FloatPoint { 0, 50 }));
    }

    NestedScrollbarContextFixture scene;
    MonotonicTime now { MonotonicTime::now() };
};

TEST_CASE(scroll_snapshots_keep_newer_unreconciled_offsets_until_they_are_adopted)
{
    LatchedWheelContextFixture fixture;
    auto& context = fixture.scene.context;

    EXPECT(fixture.wheel({ 80, 80 }, { 0, 20 }, Web::ScrollGesturePhase::Ongoing, 0).accepted);
    auto first_update = context.take_pending_async_scroll_updates();
    EXPECT(fixture.wheel({ 80, 80 }, { 0, 10 }, Web::ScrollGesturePhase::Ongoing, 10).accepted);
    auto second_update = context.take_pending_async_scroll_updates();
    VERIFY(second_update.scroll_offsets.size() == 1);
    EXPECT_EQ(second_update.scroll_offsets.first().compositor_scroll_offset, (Gfx::FloatPoint { 0, 30 }));

    // A snapshot adopting only the first update must not rewind the more recent scroll.
    auto snapshot = scroll_state_snapshot_with_offset(Compositing::SpatialNodeIndex { 1 }, { 0, -20 });
    snapshot.set_adopted_async_scroll_sequence(first_update.sequence);
    context.update_scroll_state(move(snapshot), {});
    EXPECT(fixture.wheel({ 80, 80 }, { 0, 5 }, Web::ScrollGesturePhase::Ongoing, 20).accepted);
    auto third_update = context.take_pending_async_scroll_updates();
    VERIFY(third_update.scroll_offsets.size() == 1);
    EXPECT_EQ(third_update.scroll_offsets.first().compositor_scroll_offset, (Gfx::FloatPoint { 0, 35 }));
    EXPECT_EQ(third_update.scroll_offsets.first().unadopted_scroll_delta, (Gfx::FloatPoint { 0, 5 }));

    // Once all updates are adopted, a main-thread scroll becomes the new starting offset.
    snapshot = scroll_state_snapshot_with_offset(Compositing::SpatialNodeIndex { 1 }, { 0, -70 });
    snapshot.set_adopted_async_scroll_sequence(third_update.sequence);
    context.update_scroll_state(move(snapshot), {});
    EXPECT(fixture.wheel({ 80, 80 }, { 0, 5 }, Web::ScrollGesturePhase::Ongoing, 30).accepted);
    EXPECT_EQ(fixture.take_scroll_offsets().viewport, (Gfx::FloatPoint { 0, 75 }));
}

TEST_CASE(a_mouse_wheel_tick_with_other_modifiers_starts_a_new_gesture)
{
    LatchedWheelContextFixture fixture;
    fixture.latch_gesture_to_nested_scroller_at_its_edge(Web::ScrollGesturePhase::None, Web::WheelDeltaPrecision::Discrete);

    fixture.expect_step_to_scroll_viewport_afresh({ 20, 20 }, Web::ScrollGesturePhase::None, 50, Web::UIEvents::KeyModifier::Mod_Shift, Web::WheelDeltaPrecision::Discrete);
}

TEST_CASE(momentum_within_the_grace_after_the_gesture_ended_continues_its_latch)
{
    LatchedWheelContextFixture fixture;
    fixture.latch_gesture_to_nested_scroller_at_its_edge();

    // Nothing snaps at the end of the gesture, and the navigable hosting a nested document reports the end as well.
    EXPECT(!fixture.wheel({ 20, 20 }, { 0, 0 }, Web::ScrollGesturePhase::Ended, 10).accepted);
    EXPECT(!fixture.wheel({ 20, 20 }, { 0, 0 }, Web::ScrollGesturePhase::Ended, 11).accepted);
    EXPECT_EQ(fixture.latched_scroller_node_id(), nested_scroller_node_id);

    fixture.expect_step_to_be_absorbed_by_latched_scroller({ 20, 20 }, Web::ScrollGesturePhase::Momentum, 60);
    EXPECT_EQ(fixture.latched_scroller_node_id(), nested_scroller_node_id);
}

TEST_CASE(a_latched_gesture_ignores_main_thread_wheel_regions_it_moves_over)
{
    LatchedWheelContextFixture fixture({ .main_thread_wheel_event_region_in_viewport = Gfx::FloatRect { 60, 60, 40, 40 } });

    EXPECT(!fixture.wheel({ 80, 80 }, { 0, 10 }, Web::ScrollGesturePhase::Ongoing, 0).accepted);
    EXPECT(!fixture.latched_scroller_node_id().has_value());

    EXPECT(fixture.wheel({ 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 10).accepted);
    EXPECT(fixture.wheel({ 80, 80 }, { 0, 30 }, Web::ScrollGesturePhase::Ongoing, 20).accepted);
    EXPECT_EQ(fixture.take_scroll_offsets().nested, (Gfx::FloatPoint { 0, 80 }));
}

TEST_CASE(a_pinch_pan_that_consumes_a_step_leaves_the_latch_alone)
{
    LatchedWheelContextFixture fixture;

    EXPECT(fixture.wheel({ 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 0).accepted);
    fixture.take_scroll_offsets();

    (void)fixture.scene.context.handle_pinch_event({
        .position = { Web::DevicePixels { 50 }, Web::DevicePixels { 50 } },
        .scale_delta = 1.0,
    });

    // The visual viewport pans by the whole step, which reaches no scroller.
    EXPECT(fixture.wheel({ 20, 20 }, { 0, 10 }, Web::ScrollGesturePhase::Ongoing, 20).accepted);
    EXPECT_EQ(fixture.latched_scroller_node_id(), nested_scroller_node_id);

    // What the pan cannot take goes to the latched scroller, not to what is under the cursor of the zoomed page.
    EXPECT(fixture.wheel({ 20, 20 }, { 0, 400 }, Web::ScrollGesturePhase::Ongoing, 40).accepted);
    auto offsets = fixture.take_scroll_offsets();
    EXPECT_EQ(offsets.nested, (Gfx::FloatPoint { 0, 120 }));
    EXPECT_EQ(offsets.viewport.value_or(Gfx::FloatPoint {}), Gfx::FloatPoint {});
}

static Gfx::IntRect const test_viewport_rect { 0, 0, 16, 16 };

struct PresentingContextFixture {
    Core::EventLoop event_loop;
    TestCompositorClient compositor_client;
    TestWebContentClient web_content_client;
    NonnullRefPtr<Compositor::CompositorState> compositor_state;
    Web::CompositorContextId context_id;
    Gfx::IntRect viewport_rect;

    explicit PresentingContextFixture(Gfx::IntSize viewport_size = test_viewport_rect.size())
        : compositor_state(Compositor::CompositorState::create({}))
        , context_id(Web::compositor_context_id_for_page(1))
        , viewport_rect({}, viewport_size)
    {
        compositor_state->set_client(compositor_client);
        compositor_state->create_context(context_id, 1, web_content_client);
        compositor_state->viewport_size_updated(context_id, viewport_size, Compositing::WindowResizingInProgress::No);
        VERIFY(!compositor_client.allocated_bitmap_ids.is_empty());
        release_all_buffers();
    }

    void release_all_buffers()
    {
        for (auto bitmap_id : compositor_client.allocated_bitmap_ids)
            compositor_state->presented_bitmap_ready_to_paint(context_id, bitmap_id);
    }

    void install(NonnullRefPtr<Compositing::DisplayList> display_list, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot scroll_state_snapshot = {})
    {
        compositor_state->update_display_list(context_id, move(display_list), visual_context_tree, {}, move(scroll_state_snapshot));
    }

    TestCompositorClient::PresentedFrame wait_for_frame(size_t already_presented)
    {
        VERIFY(spin_event_loop_until(event_loop, 2000, [&] { return compositor_client.presented_frames.size() > already_presented; }));
        release_all_buffers();
        return compositor_client.presented_frames.last();
    }

    bool presents_another_frame(size_t already_presented)
    {
        return spin_event_loop_until(event_loop, 100, [&] { return compositor_client.presented_frames.size() > already_presented; });
    }

    void expect_no_frame()
    {
        auto already_presented = compositor_client.presented_frames.size();
        compositor_state->present_frame(context_id, viewport_rect);
        compositor_state->present_pending_frames_for_testing();
        EXPECT_EQ(compositor_state->pending_async_present_count_for_testing(), 0u);
        EXPECT_EQ(compositor_client.presented_frames.size(), already_presented);
    }

    TestCompositorClient::PresentedFrame present(Optional<Gfx::IntRect> rect = {})
    {
        auto already_presented = compositor_client.presented_frames.size();
        compositor_state->present_frame(context_id, rect.value_or(viewport_rect));
        return wait_for_frame(already_presented);
    }

    TestCompositorClient::PresentedFrame present_without_releasing(Gfx::IntRect rect)
    {
        auto already_presented = compositor_client.presented_frames.size();
        compositor_state->present_frame(context_id, rect);
        compositor_state->present_pending_frames_for_testing();
        VERIFY(spin_event_loop_until(event_loop, 2000, [&] { return compositor_client.presented_frames.size() > already_presented; }));
        return compositor_client.presented_frames.last();
    }

#ifdef AK_OS_MACOS
    Core::IOSurfaceHandle handle_for_marking_in_use(i32 bitmap_id)
    {
        auto index = compositor_client.allocated_bitmap_ids.find_first_index(bitmap_id);
        VERIFY(index.has_value());
        return ::handle_for_marking_in_use(compositor_client.allocated_shared_image_buffers[*index]);
    }

    struct SurfaceReadByWindowServer {
        i32 bitmap_id { 0 };
        Core::IOSurfaceHandle surface;
    };

    // The client puts the frame on screen and releases the one it showed before. The window server reads a frame
    // from when it is shown until the test says it stopped, which is some time after the client replaced it.
    TestCompositorClient::PresentedFrame present_to_client_displaying_frames(Gfx::IntRect rect)
    {
        auto frame = present_without_releasing(rect);
        did_display(frame);
        return frame;
    }

    void did_display(TestCompositorClient::PresentedFrame const& frame)
    {
        auto surface = handle_for_marking_in_use(frame.bitmap_id);
        surface.increment_use_count();
        surfaces_read_by_window_server.append({ frame.bitmap_id, move(surface) });
        if (bitmap_id_displayed_by_client.has_value())
            compositor_state->presented_bitmap_ready_to_paint(context_id, *bitmap_id_displayed_by_client);
        bitmap_id_displayed_by_client = frame.bitmap_id;
    }

    void window_server_stops_reading(i32 bitmap_id)
    {
        auto index = surfaces_read_by_window_server.find_first_index_if([&](auto const& entry) { return entry.bitmap_id == bitmap_id; });
        VERIFY(index.has_value());
        surfaces_read_by_window_server[*index].surface.decrement_use_count();
        surfaces_read_by_window_server.remove(*index);
    }

    void window_server_stops_reading_every_replaced_frame()
    {
        auto bitmap_ids_of_replaced_frames = Vector<i32> {};
        for (auto const& entry : surfaces_read_by_window_server) {
            if (entry.bitmap_id != bitmap_id_displayed_by_client)
                bitmap_ids_of_replaced_frames.append(entry.bitmap_id);
        }
        for (auto bitmap_id : bitmap_ids_of_replaced_frames)
            window_server_stops_reading(bitmap_id);
    }

    ~PresentingContextFixture()
    {
        for (auto& entry : surfaces_read_by_window_server)
            entry.surface.decrement_use_count();
    }

    Vector<SurfaceReadByWindowServer> surfaces_read_by_window_server;
    Optional<i32> bitmap_id_displayed_by_client;
#endif
};

struct RasterizingContextFixture {
    TestWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context;
    Compositor::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Gfx::IntRect viewport_rect;

    explicit RasterizingContextFixture(Gfx::IntSize viewport_size = test_viewport_rect.size())
        : context(Web::CompositorContextId { 1 }, 1, client, canvas_surface_registry)
        , viewport_rect({}, viewport_size)
    {
        context.viewport_size_updated(viewport_size, Compositing::WindowResizingInProgress::No);
        auto publication = context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed);
        VERIFY(publication.has_value());
        VERIFY(context.acknowledge_presented_bitmap(publication->bitmap_ids[0]));
    }

    Optional<Compositor::ContextState::PreparedFrame> prepare(Gfx::IntRect forced_damage_rect = {})
    {
        return context.prepare_frame(display_list_player, { viewport_rect, forced_damage_rect }, nullptr);
    }

    void finish(Compositor::ContextState::PreparedFrame const& prepared_frame)
    {
        display_list_player.flush(*prepared_frame.rendered_surface);
        context.did_submit_prepared_frame(viewport_rect);
        context.did_finish_gpu_present(prepared_frame.bitmap_id);
        VERIFY(context.acknowledge_presented_bitmap(prepared_frame.bitmap_id));
    }

    Gfx::IntRect rasterize(Gfx::IntRect forced_damage_rect = {})
    {
        auto prepared_frame = prepare(forced_damage_rect);
        VERIFY(prepared_frame.has_value());
        finish(*prepared_frame);
        return prepared_frame->damage_rect;
    }

    Gfx::Color pixel(int x, int y)
    {
        return context.latest_rendered_surface()->snapshot_bitmap()->get_pixel(x, y);
    }
};

TEST_CASE(re_presenting_identical_state_does_not_submit_a_frame)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);

    auto first_frame = fixture.present();
    EXPECT_EQ(first_frame.damage_rect, fixture.viewport_rect);
    EXPECT_EQ(first_frame.content_rect, fixture.viewport_rect);

    fixture.expect_no_frame();

    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    fixture.expect_no_frame();
}

TEST_CASE(offscreen_changes_do_not_acquire_a_backing_store_or_block_later_frames)
{
    RasterizingContextFixture fixture;
    auto tree = make_translated_visual_context_tree({ 0, 100 });
    auto display_list = make_fills_display_list(tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red, in_spatial_node(1) } });
    fixture.context.install_display_list_update(display_list, tree, {});
    fixture.rasterize();
    auto surface = fixture.context.latest_rendered_surface();

    auto offscreen_tree = make_translated_visual_context_tree({ 0, 200 }, tree.structural_epoch());
    fixture.context.update_visual_context_tree(offscreen_tree, {});
    EXPECT(!fixture.prepare().has_value());
    EXPECT(!fixture.context.is_present_blocked());
    EXPECT_EQ(fixture.context.latest_rendered_surface(), surface);

    auto visible_tree = make_translated_visual_context_tree({ 0, 0 }, tree.structural_epoch());
    fixture.context.update_visual_context_tree(visible_tree, {});
    EXPECT_EQ(fixture.rasterize(), (Gfx::IntRect { 1, 1, 6, 15 }));
    EXPECT_EQ(fixture.pixel(3, 3), Gfx::Color::Red);
    EXPECT(!fixture.prepare().has_value());
    EXPECT_EQ(fixture.rasterize(fixture.viewport_rect), fixture.viewport_rect);
}

TEST_CASE(changed_command_is_repainted_within_its_damage)
{
    RasterizingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.context.install_display_list_update(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }, Gfx::Color::Green), visual_context_tree, {});
    EXPECT_EQ(fixture.rasterize(), fixture.viewport_rect);
    EXPECT_EQ(fixture.pixel(3, 3), Gfx::Color::Red);
    EXPECT_EQ(fixture.pixel(0, 0), Gfx::Color::Green);

    fixture.context.install_display_list_update(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Blue } }, Gfx::Color::Green), visual_context_tree, {});
    EXPECT_EQ(fixture.rasterize(), (Gfx::IntRect { 1, 1, 6, 6 }));
    EXPECT_EQ(fixture.pixel(3, 3), Gfx::Color::Blue);
    EXPECT_EQ(fixture.pixel(0, 0), Gfx::Color::Green);

    fixture.context.install_display_list_update(make_fills_display_list(visual_context_tree, { { { 8, 8, 4, 4 }, Gfx::Color::Blue } }, Gfx::Color::Green), visual_context_tree, {});
    EXPECT_EQ(fixture.rasterize(), (Gfx::IntRect { 1, 1, 12, 12 }));
    EXPECT_EQ(fixture.pixel(3, 3), Gfx::Color::Green);
    EXPECT_EQ(fixture.pixel(9, 9), Gfx::Color::Blue);
}

TEST_CASE(scroll_state_only_update_damages_only_moved_commands)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    Compositing::SpatialNodeIndex scroll_node_index { 1 };
    auto display_list = make_fills_display_list(visual_context_tree, {
                                                                         { { 0, 0, 4, 4 }, Gfx::Color::Green },
                                                                         { { 0, 8, 4, 2 }, Gfx::Color::Red, in_spatial_node(1) },
                                                                     });
    fixture.install(display_list, visual_context_tree, scroll_state_snapshot_with_offset(scroll_node_index, { 0, 0 }));
    fixture.present();

    fixture.compositor_state->update_scroll_state(fixture.context_id, scroll_state_snapshot_with_offset(scroll_node_index, { 0, -2 }), {});
    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 0, 5, 5, 6 }));
}

TEST_CASE(tree_only_update_damages_transformed_commands)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_translated_visual_context_tree({ 0, 0 });
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red, in_spatial_node(1) } }), visual_context_tree);
    fixture.present();

    auto translated_tree = make_translated_visual_context_tree({ 4, 0 }, visual_context_tree.structural_epoch());
    fixture.compositor_state->update_visual_context_tree(fixture.context_id, translated_tree, {});
    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 1, 1, 10, 6 }));

    fixture.compositor_state->update_visual_context_tree(fixture.context_id, make_translated_visual_context_tree({ 8, 0 }), {});
    fixture.expect_no_frame();
}

TEST_CASE(surface_clear_color_change_repaints_the_background)
{
    RasterizingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.context.install_display_list_update(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }, Gfx::Color::Green), visual_context_tree, {});
    fixture.rasterize();
    EXPECT_EQ(fixture.pixel(0, 0), Gfx::Color::Green);

    fixture.context.install_display_list_update(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }, Gfx::Color::Blue), visual_context_tree, {});
    EXPECT_EQ(fixture.rasterize(), fixture.viewport_rect);
    EXPECT_EQ(fixture.pixel(0, 0), Gfx::Color::Blue);
    EXPECT_EQ(fixture.pixel(3, 3), Gfx::Color::Red);
}

TEST_CASE(viewport_size_change_forces_full_damage)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    fixture.present();

    Gfx::IntRect resized_viewport_rect { 0, 0, 20, 20 };
    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->viewport_size_updated(fixture.context_id, resized_viewport_rect.size(), Compositing::WindowResizingInProgress::No);
    fixture.compositor_state->present_frame(fixture.context_id, resized_viewport_rect);
    auto frame = fixture.wait_for_frame(already_presented);
    EXPECT_EQ(frame.content_rect, resized_viewport_rect);
    EXPECT_EQ(frame.damage_rect, resized_viewport_rect);
}

TEST_CASE(viewport_location_change_reports_only_the_diff)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    fixture.present();

    Gfx::IntRect moved_viewport_rect { 0, 4, 16, 16 };
    auto frame = fixture.present(moved_viewport_rect);
    EXPECT_EQ(frame.content_rect, moved_viewport_rect);
    EXPECT(frame.damage_rect.is_empty());
}

TEST_CASE(resize_frames_coalesce_while_waiting_for_a_backing_store)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    fixture.viewport_rect.set_size({ 32, 32 });
    fixture.compositor_state->viewport_size_updated(fixture.context_id, fixture.viewport_rect.size(), Compositing::WindowResizingInProgress::Yes);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);

    // The client holds the initial front buffer and every buffer it is presented afterwards, so one frame per
    // remaining buffer leaves the compositor nothing to render into.
    auto buffer_count = fixture.compositor_client.allocated_bitmap_ids.size();
    for (size_t presented = 0; presented + 1 < buffer_count; ++presented) {
        fixture.compositor_state->present_frame(fixture.context_id, { 0, static_cast<int>(presented), 32, 32 });
        EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 1u);
        VERIFY(spin_event_loop_until(fixture.event_loop, 2000, [&] { return fixture.compositor_client.presented_frames.size() == presented + 1; }));
    }

    // Newer content rectangles arriving while every buffer is held coalesce into one pending frame.
    fixture.compositor_state->present_frame(fixture.context_id, { 0, 5, 32, 32 });
    Gfx::IntRect latest_viewport_rect { 0, 10, 32, 32 };
    fixture.compositor_state->present_frame(fixture.context_id, latest_viewport_rect);
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 0u);
    EXPECT_EQ(fixture.compositor_client.presented_frames.size(), buffer_count - 1);

    fixture.compositor_state->presented_bitmap_ready_to_paint(fixture.context_id, fixture.compositor_client.allocated_bitmap_ids[0]);
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 1u);
    VERIFY(spin_event_loop_until(fixture.event_loop, 2000, [&] { return fixture.compositor_client.presented_frames.size() == buffer_count; }));
    EXPECT_EQ(fixture.compositor_client.presented_frames.last().content_rect, latest_viewport_rect);

    fixture.release_all_buffers();
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 0u);
    EXPECT_EQ(fixture.compositor_client.presented_frames.size(), buffer_count);
}

#ifdef AK_OS_MACOS
TEST_CASE(a_buffer_the_window_server_still_reads_keeps_the_frame_pending_when_the_client_displays_none_of_them)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), 3u);

    // The client releases each presented buffer right away, but the window server keeps reading it, so every present
    // has to skip the buffers released before it.
    Vector<Core::IOSurfaceHandle> surfaces_still_read_by_the_window_server;
    Vector<i32> presented_bitmap_ids;
    for (int frame_index = 0; frame_index < 3; ++frame_index) {
        auto frame = fixture.present_without_releasing({ 0, frame_index, 16, 16 });
        EXPECT(!presented_bitmap_ids.contains_slow(frame.bitmap_id));
        presented_bitmap_ids.append(frame.bitmap_id);
        auto surface = fixture.handle_for_marking_in_use(frame.bitmap_id);
        EXPECT(!surface.is_in_use());
        surface.increment_use_count();
        surfaces_still_read_by_the_window_server.append(move(surface));
        fixture.compositor_state->presented_bitmap_ready_to_paint(fixture.context_id, frame.bitmap_id);
    }

    Gfx::IntRect blocked_viewport_rect { 0, 8, 16, 16 };
    fixture.compositor_state->present_frame(fixture.context_id, blocked_viewport_rect);
    for (int tick = 0; tick < 3; ++tick) {
        fixture.compositor_state->present_pending_frames_for_testing();
        EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 0u);
        EXPECT_EQ(fixture.compositor_client.presented_frames.size(), 3u);
    }

    EXPECT(fixture.compositor_client.added_bitmap_ids.is_empty());

    // No further release message arrives; the next tick alone notices the window server let go of the first buffer.
    surfaces_still_read_by_the_window_server[0].decrement_use_count();
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 1u);
    VERIFY(spin_event_loop_until(fixture.event_loop, 2000, [&] { return fixture.compositor_client.presented_frames.size() == 4; }));
    EXPECT_EQ(fixture.compositor_client.presented_frames.last().content_rect, blocked_viewport_rect);
    EXPECT_EQ(fixture.compositor_client.presented_frames.last().bitmap_id, presented_bitmap_ids[0]);

    for (auto& surface : surfaces_still_read_by_the_window_server.span().slice(1))
        surface.decrement_use_count();
}

TEST_CASE(a_frame_gets_another_backing_store_when_the_window_server_still_reads_every_released_one)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);
    auto initial_backing_store_count = fixture.compositor_client.allocated_bitmap_ids.size();

    int frame_index = 0;
    for (; frame_index < static_cast<int>(initial_backing_store_count); ++frame_index)
        fixture.present_to_client_displaying_frames({ 0, frame_index, 16, 16 });
    EXPECT(fixture.compositor_client.added_bitmap_ids.is_empty());

    Gfx::IntRect viewport_rect_of_frame_without_a_released_store { 0, frame_index, 16, 16 };
    auto frame = fixture.present_to_client_displaying_frames(viewport_rect_of_frame_without_a_released_store);
    EXPECT_EQ(fixture.compositor_client.added_bitmap_ids.size(), 1u);
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), initial_backing_store_count + 1);
    EXPECT_EQ(frame.bitmap_id, fixture.compositor_client.added_bitmap_ids.last());
    EXPECT_EQ(frame.content_rect, viewport_rect_of_frame_without_a_released_store);
}

TEST_CASE(no_backing_store_is_added_while_the_client_has_yet_to_release_the_frame_it_replaced)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), 3u);

    fixture.present_to_client_displaying_frames({ 0, 0, 16, 16 });
    fixture.present_to_client_displaying_frames({ 0, 1, 16, 16 });
    auto frame_the_client_has_yet_to_display = fixture.present_without_releasing({ 0, 2, 16, 16 });

    Gfx::IntRect pending_viewport_rect { 0, 3, 16, 16 };
    fixture.compositor_state->present_frame(fixture.context_id, pending_viewport_rect);
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 0u);
    EXPECT(fixture.compositor_client.added_bitmap_ids.is_empty());

    fixture.did_display(frame_the_client_has_yet_to_display);
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_client.added_bitmap_ids.size(), 1u);
    VERIFY(spin_event_loop_until(fixture.event_loop, 2000, [&] { return fixture.compositor_client.presented_frames.size() == 4; }));
    EXPECT_EQ(fixture.compositor_client.presented_frames.last().content_rect, pending_viewport_rect);
    EXPECT_EQ(fixture.compositor_client.presented_frames.last().bitmap_id, fixture.compositor_client.added_bitmap_ids.last());
}

TEST_CASE(backing_stores_stop_being_added_at_the_maximum_count)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);

    static constexpr size_t maximum_backing_store_count = 8;
    int frame_index = 0;
    for (; frame_index < static_cast<int>(maximum_backing_store_count); ++frame_index)
        fixture.present_to_client_displaying_frames({ 0, frame_index, 16, 16 });
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), maximum_backing_store_count);

    Gfx::IntRect pending_viewport_rect { 0, frame_index, 16, 16 };
    fixture.compositor_state->present_frame(fixture.context_id, pending_viewport_rect);
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 0u);
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), maximum_backing_store_count);

    auto bitmap_id_of_first_frame = fixture.compositor_client.presented_frames.first().bitmap_id;
    fixture.window_server_stops_reading(bitmap_id_of_first_frame);
    fixture.compositor_state->present_pending_frames_for_testing();
    VERIFY(spin_event_loop_until(fixture.event_loop, 2000, [&] { return fixture.compositor_client.presented_frames.size() == maximum_backing_store_count + 1; }));
    EXPECT_EQ(fixture.compositor_client.presented_frames.last().content_rect, pending_viewport_rect);
    EXPECT_EQ(fixture.compositor_client.presented_frames.last().bitmap_id, bitmap_id_of_first_frame);
}

TEST_CASE(added_backing_stores_are_retired_after_going_a_whole_check_interval_without_being_rendered_into)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);
    auto initial_backing_store_count = fixture.compositor_client.allocated_bitmap_ids.size();

    int frame_index = 0;
    for (; frame_index < static_cast<int>(initial_backing_store_count) + 2; ++frame_index)
        fixture.present_to_client_displaying_frames({ 0, frame_index, 16, 16 });
    EXPECT_EQ(fixture.compositor_client.added_bitmap_ids.size(), 2u);
    auto bitmap_id_displayed_by_client = *fixture.bitmap_id_displayed_by_client;
    EXPECT_EQ(bitmap_id_displayed_by_client, fixture.compositor_client.added_bitmap_ids.last());

    // Every store was rendered into since it was allocated, so the first check retires none of them.
    fixture.window_server_stops_reading_every_replaced_frame();
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT(fixture.compositor_client.retired_bitmap_ids.is_empty());

    // The store on screen stays however idle it is, so the two that go are the last ones the client released.
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT_EQ(fixture.compositor_client.retired_bitmap_ids.size(), 2u);
    EXPECT(!fixture.compositor_client.retired_bitmap_ids.contains_slow(bitmap_id_displayed_by_client));
    EXPECT(fixture.compositor_client.retired_bitmap_ids.contains_slow(fixture.compositor_client.added_bitmap_ids.first()));
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), initial_backing_store_count);

    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT_EQ(fixture.compositor_client.retired_bitmap_ids.size(), 2u);

    // The store on screen moved down as lower ones were removed; presenting goes on from the stores that are left.
    auto frame = fixture.present_to_client_displaying_frames({ 0, frame_index, 16, 16 });
    EXPECT(fixture.compositor_client.allocated_bitmap_ids.contains_slow(frame.bitmap_id));
    EXPECT_NE(frame.bitmap_id, bitmap_id_displayed_by_client);
    EXPECT_EQ(fixture.compositor_client.added_bitmap_ids.size(), 2u);
}

TEST_CASE(backing_stores_the_window_server_still_reads_are_not_retired)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);
    auto initial_backing_store_count = fixture.compositor_client.allocated_bitmap_ids.size();

    int frame_index = 0;
    for (; frame_index < static_cast<int>(initial_backing_store_count) + 1; ++frame_index)
        fixture.present_to_client_displaying_frames({ 0, frame_index, 16, 16 });
    EXPECT_EQ(fixture.compositor_client.added_bitmap_ids.size(), 1u);

    // The window server reads every released store throughout both checks.
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT(fixture.compositor_client.retired_bitmap_ids.is_empty());

    fixture.window_server_stops_reading_every_replaced_frame();
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT_EQ(fixture.compositor_client.retired_bitmap_ids.size(), 1u);
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), initial_backing_store_count);
}

TEST_CASE(backing_stores_are_not_retired_while_a_frame_is_being_rendered)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);
    auto initial_backing_store_count = fixture.compositor_client.allocated_bitmap_ids.size();

    int frame_index = 0;
    for (; frame_index < static_cast<int>(initial_backing_store_count) + 1; ++frame_index)
        fixture.present_to_client_displaying_frames({ 0, frame_index, 16, 16 });
    fixture.window_server_stops_reading_every_replaced_frame();
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT(fixture.compositor_client.retired_bitmap_ids.is_empty());

    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->present_frame(fixture.context_id, { 0, frame_index, 16, 16 });
    fixture.compositor_state->present_pending_frames_for_testing();
    EXPECT_EQ(fixture.compositor_state->pending_async_present_count_for_testing(), 1u);
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT(fixture.compositor_client.retired_bitmap_ids.is_empty());

    VERIFY(spin_event_loop_until(fixture.event_loop, 2000, [&] { return fixture.compositor_client.presented_frames.size() > already_presented; }));
    fixture.did_display(fixture.compositor_client.presented_frames.last());
    fixture.window_server_stops_reading_every_replaced_frame();
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT_EQ(fixture.compositor_client.retired_bitmap_ids.size(), 1u);
}

TEST_CASE(reallocating_backing_stores_leaves_none_to_retire)
{
    PresentingContextFixture fixture;
    fixture.compositor_state->set_display_metadata(fixture.context_id, {}, 1.0);
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree);
    auto initial_backing_store_count = fixture.compositor_client.allocated_bitmap_ids.size();

    for (int frame_index = 0; frame_index < static_cast<int>(initial_backing_store_count) + 1; ++frame_index)
        fixture.present_to_client_displaying_frames({ 0, frame_index, 16, 16 });
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), initial_backing_store_count + 1);

    fixture.compositor_state->viewport_size_updated(fixture.context_id, { 32, 32 }, Compositing::WindowResizingInProgress::No);
    EXPECT_EQ(fixture.compositor_client.allocated_bitmap_ids.size(), initial_backing_store_count);

    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    fixture.compositor_state->retire_idle_surplus_backing_stores_for_testing(fixture.context_id);
    EXPECT(fixture.compositor_client.retired_bitmap_ids.is_empty());
}
#endif

TEST_CASE(screenshot_between_presents_does_not_advance_the_baseline)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    fixture.present();

    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Blue } }), visual_context_tree);
    auto bitmap = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, fixture.viewport_rect.size()));
    Gfx::ShareableBitmap target_bitmap { bitmap, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
    EXPECT(fixture.compositor_state->request_screenshot(fixture.context_id, target_bitmap));
    EXPECT_EQ(bitmap->get_pixel(3, 3), Gfx::Color::Blue);

    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 1, 1, 6, 6 }));
}

TEST_CASE(blocked_present_does_not_advance_the_baseline)
{
    RasterizingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.context.install_display_list_update(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree, {});
    auto first_frame = fixture.prepare();
    VERIFY(first_frame.has_value());
    EXPECT_EQ(first_frame->damage_rect, fixture.viewport_rect);
    fixture.display_list_player.flush(*first_frame->rendered_surface);
    fixture.context.did_submit_prepared_frame(fixture.viewport_rect);

    fixture.context.install_display_list_update(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Blue } }), visual_context_tree, {});
    EXPECT(!fixture.prepare().has_value());
    EXPECT(fixture.context.pending_present_frame_viewport_rect().has_value());

    fixture.context.did_finish_gpu_present(first_frame->bitmap_id);
    VERIFY(fixture.context.acknowledge_presented_bitmap(first_frame->bitmap_id));
    EXPECT_EQ(fixture.rasterize(), (Gfx::IntRect { 1, 1, 6, 6 }));
}

TEST_CASE(backing_store_resize_waits_for_render_completion)
{
    RasterizingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.context.install_display_list_update(make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree, {});
    auto frame = fixture.prepare();
    VERIFY(frame.has_value());

    Gfx::IntSize resized_viewport_size { 32, 32 };
    fixture.context.viewport_size_updated(resized_viewport_size, Compositing::WindowResizingInProgress::No);
    EXPECT(!fixture.context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed).has_value());
    EXPECT_EQ(frame->rendered_surface->size(), fixture.viewport_rect.size());

    fixture.finish(*frame);
    EXPECT(fixture.context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed).has_value());
    fixture.viewport_rect.set_size(resized_viewport_size);
    auto resized_frame = fixture.prepare();
    VERIFY(resized_frame.has_value());
    EXPECT_EQ(resized_frame->rendered_surface->size(), resized_viewport_size);
    EXPECT_EQ(resized_frame->damage_rect, fixture.viewport_rect);
    fixture.finish(*resized_frame);
}

TEST_CASE(updates_between_rasters_are_diffed_against_the_last_rasterized_frame)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    fixture.present();

    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red }, { { 10, 10, 4, 4 }, Gfx::Color::Green } }), visual_context_tree);
    fixture.compositor_state->present_frame(fixture.context_id, fixture.viewport_rect);
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Blue }, { { 10, 10, 4, 4 }, Gfx::Color::Green } }), visual_context_tree);
    fixture.compositor_state->present_frame(fixture.context_id, fixture.viewport_rect);

    auto frame = fixture.wait_for_frame(already_presented);
    EXPECT(!fixture.presents_another_frame(already_presented + 1));
    EXPECT_EQ(frame.damage_rect, (Gfx::IntRect { 1, 1, 14, 14 }));
}

TEST_CASE(unbounded_change_reports_full_damage)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    fixture.present();

    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red, {}, false } }), visual_context_tree);
    EXPECT_EQ(fixture.present().damage_rect, fixture.viewport_rect);
}

TEST_CASE(canvas_content_changes_damage_the_canvas_rect)
{
    PresentingContextFixture fixture;
    auto& canvas_surface_registry = fixture.compositor_state->canvas_surface_registry();
    auto make_canvas_surface = [] { return Gfx::PaintingSurface::create_with_size({ 4, 4 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied); };
    auto canvas_id = canvas_surface_registry.create_canvas_surface(make_canvas_surface());

    auto visual_context_tree = make_visual_context_tree();
    TestDisplayList command_bytes;
    Compositing::DrawCanvas draw_canvas {
        .dst_rect = { 4, 4, 4, 4 },
        .canvas_id = canvas_id,
        .content_generation = 1,
        .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
    };
    append_display_list_command(command_bytes, draw_canvas, draw_canvas.dst_rect);
    fixture.install(decode_display_list(visual_context_tree, move(command_bytes)), visual_context_tree);
    fixture.present();
    fixture.expect_no_frame();

    canvas_surface_registry.set_canvas_surface(canvas_id, make_canvas_surface());
    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 3, 3, 6, 6 }));
    fixture.expect_no_frame();
}

static Web::MouseEvent ui_wheel_event(int x, int y, double wheel_delta_x, double wheel_delta_y, u64 id, Web::UIEvents::KeyModifier modifiers = Web::UIEvents::KeyModifier::Mod_None)
{
    auto event = mouse_event(Web::MouseEvent::Type::MouseWheel, x, y);
    event.wheel_delta_x = wheel_delta_x;
    event.wheel_delta_y = wheel_delta_y;
    event.wheel_delta_precision = Web::WheelDeltaPrecision::Precise;
    event.modifiers = modifiers;
    event.id = id;
    return event;
}

static Web::MouseEvent ui_mouse_move_event(int x, int y, u64 id)
{
    auto event = mouse_event(Web::MouseEvent::Type::MouseMove, x, y);
    event.id = id;
    return event;
}

TEST_CASE(shift_swaps_the_wheel_axes_in_the_compositor)
{
    PresentingContextFixture fixture { { 100, 100 } };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    fixture.install(make_scrollable_viewport_display_list(visual_context_tree, true, Compositing::ContextRef {}), visual_context_tree);
    fixture.present();

    // The viewport only scrolls vertically, and a horizontal delta scrolls it once Shift swaps the axes.
    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->handle_and_dispatch_mouse_event(fixture.context_id, ui_wheel_event(20, 20, 5, 0, 1, Web::UIEvents::KeyModifier::Mod_Shift));
    EXPECT_EQ(fixture.wait_for_frame(already_presented).content_rect, (Gfx::IntRect { 0, 5, 100, 100 }));

    auto const& forwarded = fixture.web_content_client.forwarded_mouse_events;
    EXPECT_EQ(forwarded.size(), 1u);
    if (forwarded.is_empty())
        return;
    EXPECT(forwarded.last().async_scroll_performed_default_action);
    // The forwarded event keeps the deltas as the UI sent them; WebContent swaps them for itself.
    EXPECT_EQ(forwarded.last().wheel_delta_x, 5.0);
    EXPECT_EQ(forwarded.last().wheel_delta_y, 0.0);
}

TEST_CASE(ui_mouse_move_over_a_compositor_painted_scrollbar_is_consumed)
{
    PresentingContextFixture fixture { { 100, 100 } };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    fixture.install(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree);
    fixture.present();

    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->handle_and_dispatch_mouse_event(fixture.context_id, ui_mouse_move_event(98, 10, 5));

    EXPECT_EQ(fixture.compositor_client.consumed_input_event_ids.size(), 1u);
    EXPECT_EQ(fixture.compositor_client.consumed_input_event_ids.last(), 5u);
    EXPECT(fixture.web_content_client.forwarded_mouse_events.is_empty());
    EXPECT_EQ(fixture.wait_for_frame(already_presented).damage_rect, fixture.viewport_rect);
}

TEST_CASE(ui_mouse_move_off_the_scrollbars_is_forwarded_unchanged)
{
    PresentingContextFixture fixture { { 100, 100 } };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    fixture.install(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree);
    fixture.present();

    fixture.compositor_state->handle_and_dispatch_mouse_event(fixture.context_id, ui_mouse_move_event(20, 20, 6));

    EXPECT(fixture.compositor_client.consumed_input_event_ids.is_empty());
    EXPECT_EQ(fixture.web_content_client.forwarded_mouse_events.size(), 1u);
    if (fixture.web_content_client.forwarded_mouse_events.is_empty())
        return;
    auto const& forwarded = fixture.web_content_client.forwarded_mouse_events.last();
    EXPECT_EQ(forwarded.id, 6u);
    EXPECT(!forwarded.async_scroll_performed_default_action);
    EXPECT(!forwarded.scrollbar_dragged_by_compositor.has_value());
}

TEST_CASE(ui_mouse_event_for_a_missing_context_is_reported_as_not_dispatched)
{
    PresentingContextFixture fixture { { 100, 100 } };

    fixture.compositor_state->handle_and_dispatch_mouse_event(Web::CompositorContextId { 999 }, ui_mouse_move_event(20, 20, 8));

    EXPECT_EQ(fixture.compositor_client.undispatched_input_event_ids.size(), 1u);
    EXPECT_EQ(fixture.compositor_client.undispatched_input_event_ids.last(), 8u);
    EXPECT(fixture.web_content_client.forwarded_mouse_events.is_empty());
}

TEST_CASE(child_context_presents_repaint_the_parent)
{
    PresentingContextFixture fixture;
    Web::CompositorContextId child_context_id { 2 };
    fixture.compositor_state->create_context(child_context_id, {}, fixture.web_content_client);
    fixture.compositor_state->set_parent_context(child_context_id, fixture.context_id);
    fixture.compositor_state->viewport_size_updated(child_context_id, { 8, 8 }, Compositing::WindowResizingInProgress::No);

    auto visual_context_tree = make_visual_context_tree();
    TestDisplayList command_bytes;
    Compositing::DrawCompositedContext draw_composited_context {
        .dst_rect = { 4, 4, 8, 8 },
        .child_context_id = child_context_id,
        .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
    };
    append_display_list_command(command_bytes, draw_composited_context, Gfx::enclosing_int_rect(draw_composited_context.dst_rect));
    fixture.install(decode_display_list(visual_context_tree, move(command_bytes)), visual_context_tree);

    auto child_visual_context_tree = make_visual_context_tree();
    Gfx::IntRect child_viewport_rect { 0, 0, 8, 8 };
    fixture.compositor_state->update_display_list(child_context_id, make_fills_display_list(child_visual_context_tree, { { child_viewport_rect, Gfx::Color::Red } }), child_visual_context_tree, {}, {});
    fixture.compositor_state->present_frame(child_context_id, child_viewport_rect);
    fixture.present();
    fixture.expect_no_frame();

    fixture.compositor_state->update_display_list(child_context_id, make_fills_display_list(child_visual_context_tree, { { child_viewport_rect, Gfx::Color::Blue } }), child_visual_context_tree, {}, {});
    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->present_frame(child_context_id, child_viewport_rect);
    EXPECT_EQ(fixture.wait_for_frame(already_presented).damage_rect, fixture.viewport_rect);
}

TEST_CASE(async_scroll_presents_report_the_damage_of_the_scrolled_content)
{
    PresentingContextFixture fixture { { 100, 100 } };
    Compositing::VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node_index = builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    auto nested_scroll_node_index = builder.append_scroll(viewport_scroll_node_index);
    auto visual_context_tree = builder.finish();
    Web::UniqueNodeID document_id { 1 };

    TestDisplayList command_bytes;
    append_display_list_command(
        command_bytes,
        Compositing::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 2 },
            .scroll_node_index = viewport_scroll_node_index,
            .parent_scroll_node_index = Compositing::VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Compositing::CompositorScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
            .is_viewport = true,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        });
    append_display_list_command(
        command_bytes,
        Compositing::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 3 },
            .scroll_node_index = nested_scroll_node_index,
            .parent_scroll_node_index = viewport_scroll_node_index,
            .scrollport_rect = { 10, 10, 40, 40 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Compositing::CompositorScrollNodeKind::Element,
            .pseudo_element_type = 0,
            .is_viewport = false,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        });
    append_display_list_command(
        command_bytes,
        Compositing::CompositorWheelHitTestTarget {
            .document_id = document_id,
            .target_scroll_node_index = nested_scroll_node_index,
            .rect = { 10, 10, 40, 40 },
        },
        {},
        in_spatial_node(viewport_scroll_node_index.value()));
    Compositing::FillRect nested_content { { 10, 10, 40, 10 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, Compositing::NO_EFFECT_NODE };
    append_display_list_command(command_bytes, nested_content, nested_content.rect, in_spatial_node(nested_scroll_node_index.value()));
    Compositing::FillRect viewport_content { { 60, 60, 10, 10 }, Gfx::Color::Green, Gfx::CompositingAndBlendingOperator::Normal, Compositing::NO_EFFECT_NODE };
    append_display_list_command(command_bytes, viewport_content, viewport_content.rect, in_spatial_node(viewport_scroll_node_index.value()));
    fixture.install(decode_display_list(visual_context_tree, move(command_bytes), {}, Compositing::DisplayList::AsyncScrollingMetadata { .viewport_rect = { 0, 0, 100, 100 } }), visual_context_tree);
    fixture.present();

    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->handle_and_dispatch_mouse_event(fixture.context_id, ui_wheel_event(20, 20, 0, 5, 1));
    EXPECT(fixture.web_content_client.forwarded_mouse_events.last().async_scroll_performed_default_action);
    auto nested_scroll_frame = fixture.wait_for_frame(already_presented);
    EXPECT_EQ(nested_scroll_frame.content_rect, fixture.viewport_rect);
    EXPECT_EQ(nested_scroll_frame.damage_rect, (Gfx::IntRect { 9, 4, 42, 17 }));

    already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->handle_and_dispatch_mouse_event(fixture.context_id, ui_wheel_event(80, 80, 0, 10, 2));
    EXPECT(fixture.web_content_client.forwarded_mouse_events.last().async_scroll_performed_default_action);
    auto viewport_scroll_frame = fixture.wait_for_frame(already_presented);
    EXPECT_EQ(viewport_scroll_frame.content_rect, (Gfx::IntRect { 0, 10, 100, 100 }));
    EXPECT_EQ(viewport_scroll_frame.damage_rect, fixture.viewport_rect);
}

// A scroll container that snaps along its y axis, with snap areas every 100 pixels.
static NonnullRefPtr<Compositing::DisplayList> make_snap_container_display_list(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::CompositorScrollNodeKind kind = Compositing::CompositorScrollNodeKind::Viewport)
{
    TestDisplayList command_bytes;
    Web::UniqueNodeID document_id { 1 };
    Compositing::SpatialNodeIndex scroll_node_index { 1 };
    VERIFY(visual_context_tree.spatial_node_count() > scroll_node_index.value());

    append_display_list_command(
        command_bytes,
        Compositing::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 2 },
            .scroll_node_index = scroll_node_index,
            .parent_scroll_node_index = Compositing::VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 400, 400 },
            .scroll_node_kind = kind,
            .pseudo_element_type = 0,
            .is_viewport = kind == Compositing::CompositorScrollNodeKind::Viewport,
            .can_be_wheel_scrolled_horizontally = true,
            .can_be_wheel_scrolled_vertically = true,
        });
    append_display_list_command(
        command_bytes,
        Compositing::CompositorWheelHitTestTarget {
            .document_id = document_id,
            .target_scroll_node_index = scroll_node_index,
            .rect = { 0, 0, 100, 100 },
        });
    append_display_list_command(
        command_bytes,
        Compositing::CompositorSnapContainer {
            .document_id = document_id,
            .scroll_node_index = scroll_node_index,
            .snapport = Web::CSSPixelRect { 0, 0, 100, 100 },
            .min_scroll_offset = Web::CSSPixelPoint { 0, 0 },
            .max_scroll_offset = Web::CSSPixelPoint { 400, 400 },
            .strictness = to_underlying(Compositing::SnapStrictness::Mandatory),
            .snaps_x = false,
            .snaps_y = true,
            .horizontal_writing_mode = true,
        });
    for (int i = 0; i < 5; ++i) {
        append_display_list_command(
            command_bytes,
            Compositing::CompositorSnapArea {
                .document_id = document_id,
                .scroll_node_index = scroll_node_index,
                .area_node_id = Web::UniqueNodeID { 10 + i },
                .pseudo_element_type = 0,
                .rect = Web::CSSPixelRect { 0, 100 * i, 100, 100 },
                .align_x = to_underlying(Compositing::SnapAlign::None),
                .align_y = to_underlying(Compositing::SnapAlign::Start),
                .always_stop = false,
            });
    }

    return decode_display_list(visual_context_tree, move(command_bytes), {},
        Compositing::DisplayList::AsyncScrollingMetadata {
            .viewport_rect = { 0, 0, 100, 100 },
            .device_pixels_per_css_pixel = 1.0,
        });
}

static constexpr Web::AsyncScrollNodeStableID snap_container_stable_id { .node_id = Web::UniqueNodeID { 2 }, .kind = Web::AsyncScrollNodeKind::Viewport, .pseudo_element_type = 0 };

struct SnapContainerContextFixture {
    RecordingWebContentClient client;
    Compositing::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::CompositorContextId { 0 }, 0, client, canvas_surface_registry };
    Compositing::AccumulatedVisualContextTree visual_context_tree { make_scrollable_viewport_visual_context_tree() };
    MonotonicTime now { MonotonicTime::now() };

    SnapContainerContextFixture(Compositing::CompositorScrollNodeKind kind = Compositing::CompositorScrollNodeKind::Viewport)
    {
        context.install_display_list_update(make_snap_container_display_list(visual_context_tree, kind), visual_context_tree, {});
    }

    Compositor::ContextState::AsyncScrollResult discrete_step(Gfx::FloatPoint delta, AK::Duration after = {})
    {
        return context.async_scroll_by(Web::UniqueNodeID { 1 }, { 50, 50 }, delta, { 0, 0, 100, 100 }, Web::WheelDeltaPrecision::Discrete, Web::ScrollGesturePhase::None, Web::UIEvents::KeyModifier::Mod_None, Compositing::AsyncScrollOperationTracking::Yes, now + after);
    }

    // Everything the context published since the last call, merged the way WebContent merges it. The context pushes
    // its pending updates to WebContent whenever it asks for a rendering update, so what was pushed is read back
    // together with what is still pending.
    Compositing::PendingAsyncScrollUpdates take_updates()
    {
        auto publications = move(client.pushed_updates);
        client.pushed_updates.clear();
        publications.append(context.take_pending_async_scroll_updates());

        Compositing::PendingAsyncScrollUpdates merged;
        for (auto& publication : publications) {
            merged.document_id = publication.document_id;
            merged.sequence = max(merged.sequence, publication.sequence);
            for (auto const& scroll_offset : publication.scroll_offsets) {
                merged.scroll_offsets.remove_all_matching([&](auto const& existing) { return existing.stable_node_id == scroll_offset.stable_node_id; });
                merged.scroll_offsets.append(scroll_offset);
            }
            merged.completed_operation_ids.extend(move(publication.completed_operation_ids));
            merged.operation_ids_taken_over_by_user_input.extend(move(publication.operation_ids_taken_over_by_user_input));
            merged.started_user_scrolls.extend(move(publication.started_user_scrolls));
        }
        return merged;
    }

    Compositing::PendingAsyncScrollUpdates finish_animations(AK::Duration after)
    {
        (void)context.advance_smooth_scroll_animations(now + after);
        VERIFY(!context.has_active_smooth_scroll_animations());
        return take_updates();
    }
};

TEST_CASE(a_wheel_gesture_ends_once_its_steps_stop_arriving)
{
    SnapContainerContextFixture fixture;

    fixture.discrete_step({ 0, 10 });
    auto updates = fixture.context.take_pending_async_scroll_updates();
    EXPECT(updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);

    // A consumed step keeps the gesture waiting for its next step as long as one that started a scroll would.
    fixture.discrete_step({ 0, 10 }, AK::Duration::from_milliseconds(200));
    fixture.take_updates();
    fixture.client.events.clear();
    fixture.context.end_scroll_step_gestures_whose_input_ran_out(fixture.now + AK::Duration::from_milliseconds(650));
    EXPECT(fixture.client.events.is_empty());
    EXPECT(!fixture.context.has_pending_async_scroll_updates());

    // The end of the gesture is pushed to WebContent, which settles the gesture on it.
    fixture.context.end_scroll_step_gestures_whose_input_ran_out(fixture.now + AK::Duration::from_milliseconds(750));
    EXPECT_EQ(fixture.client.event_sequence(), "async_scroll_updates,request_rendering_update"sv);
    auto const& pushed = fixture.client.pushed_updates.last();
    EXPECT(!pushed.user_scroll_gesture_in_progress);
    EXPECT(pushed.user_scroll_gesture_ended);
    fixture.finish_animations(AK::Duration::from_seconds(1));

    // A step arriving after the reported end travels from where the scrolling box rests rather than from the
    // 20 pixels the ended gesture asked for.
    fixture.discrete_step({ 0, 150 }, AK::Duration::from_milliseconds(1100));
    updates = fixture.take_updates();
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT_EQ(updates.started_user_scrolls.first().initial_scroll_offset, Web::CSSPixelPoint(0, 100));
    EXPECT_EQ(updates.started_user_scrolls.first().unsnapped_scroll_destination, Web::CSSPixelPoint(0, 250));
    EXPECT_EQ(updates.started_user_scrolls.first().selection.position, Web::CSSPixelPoint(0, 300));
    EXPECT(fixture.context.take_pending_async_scroll_updates().user_scroll_gesture_in_progress);
}

TEST_CASE(an_element_scroll_gesture_reports_its_document_without_a_viewport_scroll_node)
{
    SnapContainerContextFixture fixture { Compositing::CompositorScrollNodeKind::Element };

    fixture.discrete_step({ 0, 10 });
    auto updates = fixture.context.take_pending_async_scroll_updates();
    EXPECT_EQ(updates.document_id, Web::UniqueNodeID { 1 });
    EXPECT(updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);

    // WebContent needs the document identity even when the update only reports that input has ended.
    fixture.finish_animations(AK::Duration::from_milliseconds(250));
    fixture.context.end_scroll_step_gestures_whose_input_ran_out(fixture.now + AK::Duration::from_milliseconds(750));
    auto const& ended = fixture.client.pushed_updates.last();
    EXPECT_EQ(ended.document_id, Web::UniqueNodeID { 1 });
    EXPECT(!ended.user_scroll_gesture_in_progress);
    EXPECT(ended.user_scroll_gesture_ended);
    EXPECT(ended.scroll_offsets.is_empty());
}

static constexpr int arrow_key_scroll_distance_for_testing = 40;

static void target_keyboard_scrolling_at_the_snap_container(SnapContainerContextFixture& fixture)
{
    fixture.context.update_scroll_state(scroll_state_snapshot_with_offset(Compositing::SpatialNodeIndex { 1 }, { 0, 0 }),
        Compositing::KeyboardScrollState {
            .generation = 1,
            .visual_context_tree_structural_epoch = fixture.visual_context_tree.structural_epoch(),
            .target = snap_container_stable_id,
            .page_scroll_distance = 100,
            .arrow_scroll_distance = arrow_key_scroll_distance_for_testing,
        });
}

static Web::KeyEvent key_down(Web::UIEvents::KeyCode key)
{
    Web::KeyEvent event;
    event.type = Web::KeyEvent::Type::KeyDown;
    event.key = key;
    return event;
}

TEST_CASE(a_key_step_continues_from_the_destination_of_a_smooth_scroll_the_user_started_on_the_main_thread)
{
    SnapContainerContextFixture fixture;
    target_keyboard_scrolling_at_the_snap_container(fixture);

    auto main_thread_scroll = fixture.context.smooth_scroll_to(snap_container_stable_id, { 200, 0 }, { 0, 0 }, { 0, 0, 100, 100 }, Compositing::ScrollAnimationKind::SmoothScroll, Compositing::SmoothScrollInitiator::UserInput);
    EXPECT(main_thread_scroll.enqueue_result.accepted);
    fixture.take_updates();

    auto key_step = fixture.context.handle_key_event(key_down(Web::UIEvents::KeyCode::Key_Right));
    EXPECT(key_step.accepted);
    auto updates = fixture.take_updates();
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT_EQ(updates.started_user_scrolls.first().unsnapped_scroll_destination, Web::CSSPixelPoint(200 + arrow_key_scroll_distance_for_testing, 0));
}

TEST_CASE(a_key_step_takes_over_a_programmatic_smooth_scroll_from_its_presented_offset)
{
    SnapContainerContextFixture fixture;
    target_keyboard_scrolling_at_the_snap_container(fixture);

    auto programmatic_scroll = fixture.context.smooth_scroll_to(snap_container_stable_id, { 200, 0 }, { 0, 0 }, { 0, 0, 100, 100 }, Compositing::ScrollAnimationKind::SmoothScroll, Compositing::SmoothScrollInitiator::Programmatic);
    EXPECT(programmatic_scroll.enqueue_result.accepted);
    fixture.take_updates();

    auto key_step = fixture.context.handle_key_event(key_down(Web::UIEvents::KeyCode::Key_Right));
    EXPECT(key_step.accepted);
    auto updates = fixture.take_updates();
    EXPECT(updates.operation_ids_taken_over_by_user_input.contains_slow(*programmatic_scroll.enqueue_result.operation_id));
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT_EQ(updates.started_user_scrolls.first().unsnapped_scroll_destination, Web::CSSPixelPoint(arrow_key_scroll_distance_for_testing, 0));
}
