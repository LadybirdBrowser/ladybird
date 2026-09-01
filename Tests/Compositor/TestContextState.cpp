/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Queue.h>
#include <AK/Stream.h>
#include <Compositor/CompositorState.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/VisualContextTreeTestBuilder.h>
#include <LibWebView/PausedDebuggerOverlay.h>
#include <Tests/LibWeb/DisplayListTestHelpers.h>

struct TestWebContentClient final : public Compositor::CompositorStateWebContentClient {
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const&) override { }
    virtual void request_rendering_update() override { }
    virtual void rendering_opportunity(Web::Compositor::CompositorContextId, i64, double) override { }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }
};

struct TestCompositorClient final : public Compositor::CompositorStateClient {
    struct PresentedFrame {
        Gfx::IntRect content_rect;
        Gfx::IntRect damage_rect;
        i32 bitmap_id { 0 };
    };

    virtual void did_allocate_backing_stores(Web::Compositor::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&&) override
    {
        allocated_bitmap_ids = move(bitmap_ids);
    }

    virtual void did_present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id) override
    {
        presented_frames.append({ content_rect, damage_rect, bitmap_id });
    }

    Vector<i32> allocated_bitmap_ids;
    Vector<PresentedFrame> presented_frames;
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
    Web::Painting::PaintCaret caret {
        .rect = { 1, 2, 1, 10 },
        .color = Gfx::Color::Black,
        .blink_cycle_start_time_ns = 1'000'000'000,
        .should_blink = true,
    };

    EXPECT(Web::Painting::caret_is_visible_at_time(caret, 1'000'000'000));
    EXPECT(Web::Painting::caret_is_visible_at_time(caret, 1'499'999'999));
    EXPECT(!Web::Painting::caret_is_visible_at_time(caret, 1'500'000'000));
    EXPECT(!Web::Painting::caret_is_visible_at_time(caret, 1'999'999'999));
    EXPECT(Web::Painting::caret_is_visible_at_time(caret, 2'000'000'000));

    caret.should_blink = false;
    EXPECT(Web::Painting::caret_is_visible_at_time(caret, NumericLimits<i64>::max()));
}

// Round-trips a freshly built display list through the IPC encoder, so the context receives it
// the way the compositor process would.
static NonnullRefPtr<Web::Painting::DisplayList> decode_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, ByteBuffer command_bytes, Optional<Gfx::Color> surface_clear_color = {}, Optional<Web::Painting::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata = {})
{
    auto command_runs = Web::Painting::compute_display_list_command_runs(command_bytes);
    auto display_list = Web::Painting::DisplayList::create_from_command_bytes(visual_context_tree, move(command_bytes), move(command_runs));
    if (surface_clear_color.has_value())
        display_list->set_surface_clear_color(*surface_clear_color);
    if (async_scrolling_metadata.has_value())
        display_list->set_async_scrolling_metadata(*async_scrolling_metadata);

    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(*display_list));

    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<NonnullRefPtr<Web::Painting::DisplayList>>());
}

static NonnullRefPtr<Web::Painting::DisplayList> make_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Optional<Gfx::Color> color, Optional<Gfx::Color> surface_clear_color = {}, Web::Painting::ContextRef context = {})
{
    ByteBuffer command_bytes;
    if (color.has_value()) {
        auto command = Web::Painting::FillRect { { 0, 0, 4, 4 }, *color, Gfx::CompositingAndBlendingOperator::Normal };
        append_display_list_command(command_bytes, command, command.rect, context);
    }
    return decode_display_list(visual_context_tree, move(command_bytes), surface_clear_color);
}

TEST_CASE(caret_damage_uses_the_sampled_visual_context_tree)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    visual_context_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { spatial.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 20 } } } },
            },
        },
    });

    Web::Painting::PaintCaret caret {
        .rect = { 10, 10, 1, 10 },
        .color = Gfx::Color::Black,
        .blink_cycle_start_time_ns = anchor.nanoseconds(),
        .should_blink = true,
    };
    ByteBuffer command_bytes;
    append_display_list_command(command_bytes, caret, caret.rect, { spatial, Web::Painting::NO_FRAME_NODE });

    context.viewport_size_updated({ 100, 100 }, Web::Compositor::WindowResizingInProgress::No);
    context.install_display_list_update(
        decode_display_list(visual_context_tree, move(command_bytes)),
        visual_context_tree,
        {});
    EXPECT(context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500)));

    EXPECT_EQ(context.caret_damage_rect_for_testing(), Gfx::IntRect(19, 9, 3, 12));
}

TEST_CASE(visual_context_trees_round_trip_through_ipc_and_reject_corrupted_bytes)
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto scroll_node = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    builder.append_clip_frame(Web::Painting::NO_FRAME_NODE, scroll_node, { 1, 2, 3, 4 });
    auto visual_context_tree = builder.finish();

    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(visual_context_tree));
    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    auto decoded_tree = MUST(decoder.decode<Web::Painting::AccumulatedVisualContextTree>());
    EXPECT_EQ(decoded_tree.structural_epoch(), visual_context_tree.structural_epoch());
    EXPECT_EQ(decoded_tree.spatial_node_count(), 2u);
    EXPECT_EQ(decoded_tree.frame_node_count(), 1u);
    EXPECT_EQ(decoded_tree.serialize_to_bytes(), visual_context_tree.serialize_to_bytes());

    auto corrupted_bytes = visual_context_tree.serialize_to_bytes();
    corrupted_bytes[0] ^= 0xff;
    EXPECT(Web::Painting::AccumulatedVisualContextTree::from_serialized_bytes(corrupted_bytes).is_error());
}

static Web::Painting::AccumulatedVisualContextTree make_visual_context_tree()
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    return builder.finish();
}

static Web::Painting::AccumulatedVisualContextTree make_scrollable_viewport_visual_context_tree()
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    return builder.finish();
}

static NonnullRefPtr<Web::Painting::DisplayList> make_scrollable_viewport_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, bool with_viewport_scrollbar = true, Optional<Web::Painting::ContextRef> wheel_hit_test_context = {})
{
    ByteBuffer command_bytes;
    Web::UniqueNodeID document_id { 1 };
    Web::Painting::SpatialNodeIndex scroll_node_index { 1 };
    VERIFY(visual_context_tree.spatial_node_count() > scroll_node_index.value());

    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 2 },
            .scroll_node_index = scroll_node_index,
            .parent_scroll_node_index = Web::Painting::VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Web::Painting::CompositorScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
            .is_viewport = true,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
            .snaps_scroll_position_horizontally = false,
            .snaps_scroll_position_vertically = false,
        });

    if (wheel_hit_test_context.has_value()) {
        append_display_list_command(
            command_bytes,
            Web::Painting::CompositorWheelHitTestTarget {
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
            Web::Painting::CompositorViewportScrollbar {
                .document_id = document_id,
                .scroll_node_index = scroll_node_index,
                .gutter_rect = { 96, 0, 4, 100 },
                .thumb_rect = { 98, 0, 2, 20 },
                .expanded_gutter_rect = { 92, 0, 8, 100 },
                .expanded_thumb_rect = { 94, 0, 6, 20 },
                .scroll_size = 0.8,
                .expanded_scroll_size = 0.8,
                .min_scroll_offset = 0,
                .max_scroll_offset = 100,
                .thumb_color = Gfx::Color::Black,
                .track_color = Gfx::Color::Transparent,
                .vertical = true,
            });
    }

    return decode_display_list(visual_context_tree, move(command_bytes), {},
        Web::Painting::DisplayList::AsyncScrollingMetadata {
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
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    auto visual_context_tree = Web::Painting::VisualContextTreeTestBuilder().finish();
    auto viewport_rect = Gfx::IntRect { 0, 0, 4, 4 };

    context.viewport_size_updated(viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
    auto publication = context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed);
    VERIFY(publication.has_value());

    auto paint_frame = [&](NonnullRefPtr<Web::Painting::DisplayList> display_list) {
        context.install_display_list_update(move(display_list), visual_context_tree, {});
        context.queue_present_frame({ viewport_rect, viewport_rect });
        EXPECT(context.present_synchronously(display_list_player, nullptr));
    };

    // Paint both backing stores red before reusing the first one for a frame with no commands.
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

TEST_CASE(visual_animations_advance_without_a_web_content_update)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto frame = builder.append_effects_frame(Web::Painting::NO_FRAME_NODE, spatial);
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    visual_context_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { spatial.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 8 } } } },
            },
        },
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Opacity,
            .visual_context_node_indices = { frame.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, 1.0f },
                { 1, {}, 0.0f },
            },
        },
    });

    Gfx::IntRect viewport_rect { 0, 0, 12, 4 };
    context.viewport_size_updated(viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
    VERIFY(context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed).has_value());
    context.install_display_list_update(
        make_display_list(visual_context_tree, Gfx::Color::Red, Gfx::Color::Transparent, { spatial, frame }),
        visual_context_tree,
        {});

    EXPECT(context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(250)));
    EXPECT(context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500)));
    EXPECT(context.has_sampled_visual_animation_values_for_testing());
    EXPECT_EQ(context.visual_context_tree_copy_count_for_testing(), 0u);
    context.queue_present_frame({ viewport_rect, viewport_rect });
    EXPECT(context.present_synchronously(display_list_player, nullptr));

    auto bitmap = context.latest_rendered_surface()->snapshot_bitmap();
    EXPECT_EQ(bitmap->get_pixel(0, 0), Gfx::Color::Transparent);
    auto animated_pixel = bitmap->get_pixel(4, 0);
    EXPECT_EQ(animated_pixel.red(), 128);
    EXPECT_EQ(animated_pixel.green(), 0);
    EXPECT_EQ(animated_pixel.blue(), 0);
    EXPECT_EQ(animated_pixel.alpha(), 128);

    context.update_visual_context_tree(visual_context_tree, {});
    EXPECT(!context.has_sampled_visual_animation_values_for_testing());
    auto const& restored_tree = context.visual_context_tree_for_testing();
    auto restored_translation = restored_tree.accumulated_matrix(spatial, {}, Web::Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes)[0, 3];
    EXPECT_EQ(restored_translation, 0.0f);
    EXPECT_EQ(restored_tree.effects_opacity(frame), Optional<float> { 1.0f });
}

TEST_CASE(wheel_hit_testing_uses_the_current_visual_animation_tree)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto scroll_node = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    auto animated_transform = builder.append_transform(scroll_node, Gfx::FloatMatrix4x4::identity());
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    visual_context_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { animated_transform.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 20 } } } },
            },
        },
    });

    context.install_display_list_update(
        make_scrollable_viewport_display_list(visual_context_tree, false, Web::Painting::ContextRef { animated_transform, Web::Painting::NO_FRAME_NODE }),
        visual_context_tree,
        {});

    EXPECT(context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500)));
    auto result = context.async_scroll_by(
        Web::UniqueNodeID { 1 },
        { 20, 20 },
        { 0, 10 },
        { 0, 0, 100, 100 },
        Web::Compositor::SnapContainerHandling::ScrollOnCompositor,
        Web::Compositor::AsyncScrollOperationTracking::No);
    EXPECT(result.enqueue_result.accepted);
}

TEST_CASE(wheel_hit_testing_ignores_targets_from_a_larger_visual_context_tree)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto scroll_node = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    auto removed_transform = builder.append_transform(scroll_node, Gfx::FloatMatrix4x4::identity());
    auto visual_context_tree = builder.finish();

    context.install_display_list_update(
        make_scrollable_viewport_display_list(visual_context_tree, false, Web::Painting::ContextRef { removed_transform, Web::Painting::NO_FRAME_NODE }),
        visual_context_tree,
        {});

    auto smaller_visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(
        make_scrollable_viewport_display_list(smaller_visual_context_tree, false, Web::Painting::ContextRef { removed_transform, Web::Painting::NO_FRAME_NODE }),
        smaller_visual_context_tree,
        {});

    auto result = context.async_scroll_by(
        Web::UniqueNodeID { 1 },
        { 20, 20 },
        { 0, 10 },
        { 0, 0, 100, 100 },
        Web::Compositor::SnapContainerHandling::ScrollOnCompositor,
        Web::Compositor::AsyncScrollOperationTracking::No);
    EXPECT(result.enqueue_result.accepted);
}

TEST_CASE(pinch_zoom_copies_the_visual_context_tree_once_per_update)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
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

TEST_CASE(culled_initial_animation_content_becomes_visible)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Web::Compositor::VisualAnimationTransformOperation initial_scale {
        Web::Compositor::VisualAnimationTransformOperationKind::Scale,
        { 0 },
    };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto opacity_spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto opacity_frame = builder.append_effects_frame(Web::Painting::NO_FRAME_NODE, opacity_spatial, 0);
    auto scale_spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, initial_scale.to_matrix());
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    visual_context_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Opacity,
            .visual_context_node_indices = { opacity_frame.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .iteration_count = 1,
            .easing = {},
            .keyframes = {
                { 0, {}, 0.0f },
                { 1, {}, 1.0f },
            },
        },
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { scale_spatial.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .iteration_count = 1,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::Scale, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::Scale, { 1 } } } },
            },
        },
    });

    ByteBuffer command_bytes;
    auto opacity_command = Web::Painting::FillRect { { 0, 0, 4, 4 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal };
    append_display_list_command(command_bytes, opacity_command, opacity_command.rect, { opacity_spatial, opacity_frame });
    auto scale_command = Web::Painting::FillRect { { 8, 0, 4, 4 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal };
    append_display_list_command(command_bytes, scale_command, scale_command.rect, { scale_spatial, Web::Painting::NO_FRAME_NODE });

    Gfx::IntRect viewport_rect { 0, 0, 12, 4 };
    context.viewport_size_updated(viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
    VERIFY(context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed).has_value());
    context.install_display_list_update(
        decode_display_list(visual_context_tree, move(command_bytes), Gfx::Color::Transparent),
        visual_context_tree,
        {});

    EXPECT(context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500)));
    context.queue_present_frame({ viewport_rect, viewport_rect });
    EXPECT(context.present_synchronously(display_list_player, nullptr));

    auto bitmap = context.latest_rendered_surface()->snapshot_bitmap();
    auto opacity_pixel = bitmap->get_pixel(0, 0);
    EXPECT_EQ(opacity_pixel.red(), 128);
    EXPECT_EQ(opacity_pixel.alpha(), 128);
    EXPECT_EQ(bitmap->get_pixel(4, 0), Gfx::Color::Red);
}

TEST_CASE(finite_visual_animations_stop_after_their_terminal_sample)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    visual_context_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { spatial.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .iteration_count = 1,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 8 } } } },
            },
        },
    });

    Gfx::IntRect viewport_rect { 0, 0, 12, 4 };
    context.viewport_size_updated(viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
    VERIFY(context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed).has_value());
    context.install_display_list_update(
        make_display_list(visual_context_tree, Gfx::Color::Red, Gfx::Color::Transparent, { spatial, Web::Painting::NO_FRAME_NODE }),
        visual_context_tree,
        {});

    EXPECT(context.has_active_visual_animations());
    EXPECT(context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500)));
    EXPECT(context.has_active_visual_animations());
    EXPECT(!context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(1000)));
    EXPECT(!context.has_active_visual_animations());

    context.queue_present_frame({ viewport_rect, viewport_rect });
    EXPECT(context.present_synchronously(display_list_player, nullptr));
    auto bitmap = context.latest_rendered_surface()->snapshot_bitmap();
    EXPECT_EQ(bitmap->get_pixel(0, 0), Gfx::Color::Transparent);
    EXPECT_EQ(bitmap->get_pixel(8, 0), Gfx::Color::Red);
}

TEST_CASE(oversized_backing_stores_are_rejected)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed({ 40'000, 40'000 }, Web::Compositor::WindowResizingInProgress::No);
    VERIFY(allocation.has_value());

    auto publication = manager.allocate_backing_stores(*allocation, {}, true, Compositor::BackingStoreManager::GpuSharing::Disallowed);

    EXPECT(!publication.has_value());
    EXPECT(!manager.is_valid());
}
TEST_CASE(viewport_scrollbar_collapses_when_drag_is_released_away_from_scrollbar)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
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
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
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

TEST_CASE(context_visibility_and_pending_frame_state)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 1, client, canvas_surface_registry, false };
    auto viewport_rect = Gfx::IntRect { 0, 0, 4, 4 };

    EXPECT(!context.set_visibility(Web::Compositor::ContextVisibility::Visible));
    EXPECT(context.set_visibility(Web::Compositor::ContextVisibility::Hidden));
    EXPECT(!context.set_visibility(Web::Compositor::ContextVisibility::Hidden));
    EXPECT(context.set_visibility(Web::Compositor::ContextVisibility::Visible));
    EXPECT(!context.pending_present_frame_viewport_rect().has_value());

    context.viewport_size_updated(viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
    VERIFY(context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed).has_value());
    context.queue_present_frame({ viewport_rect, { 0, 0, 2, 2 } });
    EXPECT_EQ(context.pending_present_frame_viewport_rect(), viewport_rect);
    EXPECT(context.can_schedule_pending_present_frame_if_unblocked());
    context.mark_pending_present_frame_scheduled();
    EXPECT(!context.can_schedule_pending_present_frame_if_unblocked());
    context.unschedule_pending_present_frame();
    EXPECT(context.can_schedule_pending_present_frame_if_unblocked());
}

TEST_CASE(hidden_context_coalesces_presents_and_presents_once_when_shown)
{
    Core::EventLoop event_loop;
    TestCompositorClient compositor_client;
    TestWebContentClient web_content_client;
    auto compositor_state = Compositor::CompositorState::create({}, false);
    compositor_state->set_client(compositor_client);

    u64 page_id = 1;
    auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
    auto viewport_rect = Gfx::IntRect { 0, 0, 4, 4 };
    auto visual_context_tree = Web::Painting::VisualContextTreeTestBuilder().finish();

    compositor_state->create_context(context_id, page_id, web_content_client);
    compositor_state->viewport_size_updated(context_id, viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
    EXPECT(!compositor_client.allocated_bitmap_ids.is_empty());
    compositor_state->update_display_list(context_id, make_display_list(visual_context_tree, Gfx::Color::Red), visual_context_tree, {}, {});

    compositor_state->set_context_visibility(context_id, Web::Compositor::ContextVisibility::Hidden);
    compositor_state->present_frame(context_id, viewport_rect);
    compositor_state->present_frame(context_id, viewport_rect);
    compositor_state->presented_bitmap_ready_to_paint(context_id, compositor_client.allocated_bitmap_ids[0]);
    EXPECT(!spin_event_loop_until(event_loop, 100, [&] { return !compositor_client.presented_frames.is_empty(); }));

    compositor_state->set_context_visibility(context_id, Web::Compositor::ContextVisibility::Visible);
    EXPECT(spin_event_loop_until(event_loop, 2000, [&] { return !compositor_client.presented_frames.is_empty(); }));
    EXPECT(!spin_event_loop_until(event_loop, 100, [&] { return compositor_client.presented_frames.size() > 1; }));
    EXPECT_EQ(compositor_client.presented_frames.size(), 1u);
    EXPECT_EQ(compositor_client.presented_frames[0].damage_rect, viewport_rect);
}

TEST_CASE(dragging_a_viewport_scrollbar_reports_a_user_scroll_gesture_until_it_is_released)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
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

TEST_CASE(losing_the_scrollbar_a_drag_holds_ends_its_user_scroll_gesture)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
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
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };

    context.viewport_size_updated({ 640, 480 }, Web::Compositor::WindowResizingInProgress::No);
    context.did_submit_prepared_frame({ 12, 18, 640, 480 });

    context.viewport_size_updated({ 800, 600 }, Web::Compositor::WindowResizingInProgress::Yes);
    EXPECT_EQ(context.viewport_rect_for_ui_overlay(), (Gfx::IntRect { 12, 18, 800, 600 }));

    context.queue_present_frame({ { 30, 40, 800, 600 }, { 0, 0, 800, 600 } });
    context.viewport_size_updated({ 1024, 768 }, Web::Compositor::WindowResizingInProgress::Yes);
    EXPECT_EQ(context.viewport_rect_for_ui_overlay(), (Gfx::IntRect { 30, 40, 1024, 768 }));
}

TEST_CASE(ui_overlay_hover_changes_require_repainting)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };

    EXPECT(context.set_paused_debugger_overlay(true, 1.0, {}, {}));
    EXPECT(!context.set_paused_debugger_overlay(true, 1.0, {}, {}));
    EXPECT(context.set_paused_debugger_overlay(true, 1.0, {}, WebView::PausedDebuggerOverlayAction::StepOver));
    EXPECT(!context.set_paused_debugger_overlay(true, 1.0, {}, WebView::PausedDebuggerOverlayAction::StepOver));
    EXPECT(context.set_paused_debugger_overlay(true, 1.0, {}, {}));
}

struct Fill {
    Gfx::IntRect rect;
    Gfx::Color color;
    Web::Painting::ContextRef context {};
    bool bounded { true };
};

static NonnullRefPtr<Web::Painting::DisplayList> make_fills_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Vector<Fill> const& fills, Optional<Gfx::Color> surface_clear_color = {}, Optional<Web::Painting::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata = {})
{
    ByteBuffer command_bytes;
    for (auto const& fill : fills) {
        Web::Painting::FillRect command { fill.rect, fill.color, Gfx::CompositingAndBlendingOperator::Normal };
        append_display_list_command(command_bytes, command, fill.bounded ? Optional<Gfx::IntRect> { fill.rect } : Optional<Gfx::IntRect> {}, fill.context);
    }
    return decode_display_list(visual_context_tree, move(command_bytes), surface_clear_color, async_scrolling_metadata);
}

static Web::Painting::AccumulatedVisualContextTree make_translated_visual_context_tree(Gfx::FloatPoint translation, Optional<u64> structural_epoch = {})
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::translation_matrix(Gfx::FloatVector3 { translation.x(), translation.y(), 0 }));
    if (structural_epoch.has_value())
        return builder.finish_with_structural_epoch(*structural_epoch);
    return builder.finish();
}

static Web::Painting::ScrollStateSnapshot scroll_state_snapshot_with_offset(Web::Painting::SpatialNodeIndex index, Gfx::FloatPoint device_offset)
{
    Web::Painting::ScrollStateSnapshot scroll_state_snapshot;
    scroll_state_snapshot.set_device_offset_for_index(index, device_offset);
    return scroll_state_snapshot;
}

static constexpr Web::Painting::ContextRef in_spatial_node(u32 index)
{
    return { Web::Painting::SpatialNodeIndex { index }, Web::Painting::NO_FRAME_NODE };
}

static Gfx::IntRect const test_viewport_rect { 0, 0, 16, 16 };

struct PresentingContextFixture {
    Core::EventLoop event_loop;
    TestCompositorClient compositor_client;
    TestWebContentClient web_content_client;
    NonnullRefPtr<Compositor::CompositorState> compositor_state;
    Web::Compositor::CompositorContextId context_id;
    Gfx::IntRect viewport_rect;

    explicit PresentingContextFixture(Gfx::IntSize viewport_size = test_viewport_rect.size(), bool async_scrolling_enabled = false)
        : compositor_state(Compositor::CompositorState::create({}, async_scrolling_enabled))
        , context_id(Web::Compositor::compositor_context_id_for_page(1))
        , viewport_rect({}, viewport_size)
    {
        compositor_state->set_client(compositor_client);
        compositor_state->create_context(context_id, 1, web_content_client);
        compositor_state->viewport_size_updated(context_id, viewport_size, Web::Compositor::WindowResizingInProgress::No);
        VERIFY(!compositor_client.allocated_bitmap_ids.is_empty());
        release_all_buffers();
    }

    void release_all_buffers()
    {
        for (auto bitmap_id : compositor_client.allocated_bitmap_ids)
            compositor_state->presented_bitmap_ready_to_paint(context_id, bitmap_id);
    }

    void install(NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Web::Painting::ScrollStateSnapshot scroll_state_snapshot = {})
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

    TestCompositorClient::PresentedFrame present(Optional<Gfx::IntRect> rect = {})
    {
        auto already_presented = compositor_client.presented_frames.size();
        compositor_state->present_frame(context_id, rect.value_or(viewport_rect));
        return wait_for_frame(already_presented);
    }
};

struct RasterizingContextFixture {
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context;
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Gfx::IntRect viewport_rect;

    explicit RasterizingContextFixture(Gfx::IntSize viewport_size = test_viewport_rect.size())
        : context(1, client, canvas_surface_registry, false)
        , viewport_rect({}, viewport_size)
    {
        context.viewport_size_updated(viewport_size, Web::Compositor::WindowResizingInProgress::No);
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

TEST_CASE(re_presenting_identical_state_reports_empty_damage)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);

    auto first_frame = fixture.present();
    EXPECT_EQ(first_frame.damage_rect, fixture.viewport_rect);
    EXPECT_EQ(first_frame.content_rect, fixture.viewport_rect);

    EXPECT(fixture.present().damage_rect.is_empty());

    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    EXPECT(fixture.present().damage_rect.is_empty());
}

TEST_CASE(changed_command_reports_its_inflated_rect)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }), visual_context_tree);
    fixture.present();

    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Blue } }), visual_context_tree);
    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 1, 1, 6, 6 }));

    fixture.install(make_fills_display_list(visual_context_tree, { { { 8, 8, 4, 4 }, Gfx::Color::Blue } }), visual_context_tree);
    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 1, 1, 12, 12 }));
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
}

TEST_CASE(scroll_state_only_update_damages_only_moved_commands)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    Web::Painting::SpatialNodeIndex scroll_node_index { 1 };
    auto display_list = make_fills_display_list(visual_context_tree, {
                                                                         { { 0, 0, 4, 4 }, Gfx::Color::Green },
                                                                         { { 0, 8, 4, 2 }, Gfx::Color::Red, in_spatial_node(1) },
                                                                     });
    fixture.install(display_list, visual_context_tree, scroll_state_snapshot_with_offset(scroll_node_index, { 0, 0 }));
    fixture.present();

    fixture.compositor_state->update_scroll_state(fixture.context_id, scroll_state_snapshot_with_offset(scroll_node_index, { 0, -2 }));
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
    EXPECT(fixture.present().damage_rect.is_empty());
}

TEST_CASE(surface_clear_color_change_forces_full_damage)
{
    PresentingContextFixture fixture;
    auto visual_context_tree = make_visual_context_tree();
    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }, Gfx::Color::Green), visual_context_tree);
    fixture.present();

    fixture.install(make_fills_display_list(visual_context_tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red } }, Gfx::Color::Blue), visual_context_tree);
    EXPECT_EQ(fixture.present().damage_rect, fixture.viewport_rect);
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
    fixture.compositor_state->viewport_size_updated(fixture.context_id, resized_viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
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
    ByteBuffer command_bytes;
    Web::Painting::DrawCanvas draw_canvas {
        .dst_rect = { 4, 4, 4, 4 },
        .canvas_id = canvas_id,
        .content_generation = 1,
        .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
    };
    append_display_list_command(command_bytes, draw_canvas, draw_canvas.dst_rect);
    fixture.install(decode_display_list(visual_context_tree, move(command_bytes)), visual_context_tree);
    fixture.present();
    EXPECT(fixture.present().damage_rect.is_empty());

    canvas_surface_registry.set_canvas_surface(canvas_id, make_canvas_surface());
    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 3, 3, 6, 6 }));
    EXPECT(fixture.present().damage_rect.is_empty());
}

TEST_CASE(compositor_initiated_presents_request_full_damage)
{
    PresentingContextFixture fixture { { 100, 100 }, true };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    fixture.install(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree);
    fixture.present();

    auto already_presented = fixture.compositor_client.presented_frames.size();
    EXPECT(fixture.compositor_state->handle_mouse_event(fixture.context_id, mouse_event(Web::MouseEvent::Type::MouseMove, 98, 10)));
    EXPECT_EQ(fixture.wait_for_frame(already_presented).damage_rect, fixture.viewport_rect);
}

TEST_CASE(child_context_presents_repaint_the_parent)
{
    PresentingContextFixture fixture;
    Web::Compositor::CompositorContextId child_context_id { 2 };
    fixture.compositor_state->create_context(child_context_id, {}, fixture.web_content_client);
    fixture.compositor_state->set_parent_context(child_context_id, fixture.context_id);
    fixture.compositor_state->viewport_size_updated(child_context_id, { 8, 8 }, Web::Compositor::WindowResizingInProgress::No);

    auto visual_context_tree = make_visual_context_tree();
    ByteBuffer command_bytes;
    Web::Painting::DrawCompositedContext draw_composited_context {
        .dst_rect = { 4, 4, 8, 8 },
        .child_context_id = child_context_id,
        .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
    };
    append_display_list_command(command_bytes, draw_composited_context, draw_composited_context.dst_rect);
    fixture.install(decode_display_list(visual_context_tree, move(command_bytes)), visual_context_tree);

    auto child_visual_context_tree = make_visual_context_tree();
    Gfx::IntRect child_viewport_rect { 0, 0, 8, 8 };
    fixture.compositor_state->update_display_list(child_context_id, make_fills_display_list(child_visual_context_tree, { { child_viewport_rect, Gfx::Color::Red } }), child_visual_context_tree, {}, {});
    fixture.compositor_state->present_frame(child_context_id, child_viewport_rect);
    fixture.present();
    EXPECT(fixture.present().damage_rect.is_empty());

    fixture.compositor_state->update_display_list(child_context_id, make_fills_display_list(child_visual_context_tree, { { child_viewport_rect, Gfx::Color::Blue } }), child_visual_context_tree, {}, {});
    auto already_presented = fixture.compositor_client.presented_frames.size();
    fixture.compositor_state->present_frame(child_context_id, child_viewport_rect);
    EXPECT_EQ(fixture.wait_for_frame(already_presented).damage_rect, fixture.viewport_rect);
}

TEST_CASE(async_scroll_presents_report_the_damage_of_the_scrolled_content)
{
    PresentingContextFixture fixture { { 100, 100 }, true };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node_index = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    auto nested_scroll_node_index = builder.append_scroll(viewport_scroll_node_index);
    auto visual_context_tree = builder.finish();
    Web::UniqueNodeID document_id { 1 };

    ByteBuffer command_bytes;
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 2 },
            .scroll_node_index = viewport_scroll_node_index,
            .parent_scroll_node_index = Web::Painting::VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Web::Painting::CompositorScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
            .is_viewport = true,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
            .snaps_scroll_position_horizontally = false,
            .snaps_scroll_position_vertically = false,
        });
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 3 },
            .scroll_node_index = nested_scroll_node_index,
            .parent_scroll_node_index = viewport_scroll_node_index,
            .scrollport_rect = { 10, 10, 40, 40 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Web::Painting::CompositorScrollNodeKind::Element,
            .pseudo_element_type = 0,
            .is_viewport = false,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
            .snaps_scroll_position_horizontally = false,
            .snaps_scroll_position_vertically = false,
        });
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorWheelHitTestTarget {
            .document_id = document_id,
            .target_scroll_node_index = nested_scroll_node_index,
            .rect = { 10, 10, 40, 40 },
        },
        {},
        in_spatial_node(viewport_scroll_node_index.value()));
    Web::Painting::FillRect nested_content { { 10, 10, 40, 10 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal };
    append_display_list_command(command_bytes, nested_content, nested_content.rect, in_spatial_node(nested_scroll_node_index.value()));
    Web::Painting::FillRect viewport_content { { 60, 60, 10, 10 }, Gfx::Color::Green, Gfx::CompositingAndBlendingOperator::Normal };
    append_display_list_command(command_bytes, viewport_content, viewport_content.rect, in_spatial_node(viewport_scroll_node_index.value()));
    fixture.install(decode_display_list(visual_context_tree, move(command_bytes), {}, Web::Painting::DisplayList::AsyncScrollingMetadata { .viewport_rect = { 0, 0, 100, 100 } }), visual_context_tree);
    fixture.present();

    auto already_presented = fixture.compositor_client.presented_frames.size();
    EXPECT(fixture.compositor_state->async_scroll_by(fixture.context_id, { 20, 20 }, { 0, 5 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor));
    auto nested_scroll_frame = fixture.wait_for_frame(already_presented);
    EXPECT_EQ(nested_scroll_frame.content_rect, fixture.viewport_rect);
    EXPECT_EQ(nested_scroll_frame.damage_rect, (Gfx::IntRect { 9, 4, 42, 17 }));

    already_presented = fixture.compositor_client.presented_frames.size();
    EXPECT(fixture.compositor_state->async_scroll_by(fixture.context_id, { 80, 80 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor));
    auto viewport_scroll_frame = fixture.wait_for_frame(already_presented);
    EXPECT_EQ(viewport_scroll_frame.content_rect, (Gfx::IntRect { 0, 10, 100, 100 }));
    EXPECT_EQ(viewport_scroll_frame.damage_rect, fixture.viewport_rect);
}

TEST_CASE(clip_path_frames_round_trip_through_serialized_tree_bytes)
{
    Gfx::Path star;
    star.move_to({ 65, 0 });
    star.line_to({ 35, 80 });
    star.line_to({ 105, 30 });
    star.line_to({ 25, 30 });
    star.line_to({ 95, 80 });
    star.close();
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto clip_path_frame = builder.append_clip_path_frame(Web::Painting::NO_FRAME_NODE, Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, star, { 25, 0, 80, 80 }, Gfx::WindingRule::EvenOdd);
    auto visual_context_tree = builder.finish();

    auto serialized_bytes = visual_context_tree.serialize_to_bytes();
    auto decoded_tree = MUST(Web::Painting::AccumulatedVisualContextTree::from_serialized_bytes(serialized_bytes));
    EXPECT_EQ(decoded_tree.serialize_to_bytes(), serialized_bytes);

    Web::Painting::ScrollStateSnapshot unscrolled;
    Web::Painting::ContextRef clip_path_context { Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, clip_path_frame };
    EXPECT(decoded_tree.transform_point_for_hit_test(clip_path_context, Gfx::FloatPoint { 65, 5 }, unscrolled).has_value());
    EXPECT(!decoded_tree.transform_point_for_hit_test(clip_path_context, Gfx::FloatPoint { 26, 1 }, unscrolled).has_value());
    EXPECT(!decoded_tree.transform_point_for_hit_test(clip_path_context, Gfx::FloatPoint { 10, 40 }, unscrolled).has_value());
}

TEST_CASE(visual_animation_samples_derive_a_tree_and_leave_the_source_untouched)
{
    auto matrix = Gfx::FloatMatrix4x4::identity();
    matrix[0, 3] = 12;
    matrix[1, 3] = 12;
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, matrix, { 12, 12 });
    auto frame = builder.append_effects_frame(Web::Painting::NO_FRAME_NODE, spatial, 0.75f);
    auto tree = builder.finish();
    tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { spatial.value() },
            .local_time_at_anchor_ms = 500,
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 8 } } } },
            },
        },
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Opacity,
            .visual_context_node_indices = { frame.value() },
            .local_time_at_anchor_ms = 500,
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, 1.0f },
                { 1, {}, 0.0f },
            },
        },
    });

    auto include_viewport = Web::Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes;
    auto sampled_tree = tree.with_visual_animation_samples(0);
    EXPECT_EQ(sampled_tree.structural_epoch(), tree.structural_epoch());
    auto sampled_translation = sampled_tree.accumulated_matrix(spatial, {}, include_viewport)[0, 3];
    EXPECT_EQ(sampled_translation, 4.0f);
    EXPECT_EQ(sampled_tree.effects_opacity(frame), Optional<float> { 0.5f });
    EXPECT_EQ(sampled_tree.visual_animations().size(), 2u);

    auto source_translation = tree.accumulated_matrix(spatial, {}, include_viewport)[0, 3];
    EXPECT_EQ(source_translation, 12.0f);
    EXPECT_EQ(tree.effects_opacity(frame), Optional<float> { 0.75f });
}
