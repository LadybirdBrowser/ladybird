/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Math.h>
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
#include <LibWeb/Painting/DisplayListDamage.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/VisualContextTreeTestBuilder.h>
#include <LibWebView/PausedDebuggerOverlay.h>
#include <Tests/LibWeb/DisplayListTestHelpers.h>

struct TestWebContentClient final : public Compositor::CompositorStateWebContentClient {
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const&) override { }
    virtual void dispatch_key_event_to_web_content(u64, Web::KeyEvent const&) override { }
    virtual void request_rendering_update() override { }
    virtual void rendering_opportunity(Web::Compositor::CompositorContextId, i64, double) override { }
    virtual void async_scroll_updates(Web::Compositor::CompositorContextId, Web::Compositor::PendingAsyncScrollUpdates const&) override { }
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

static NonnullRefPtr<Web::Painting::DisplayList> make_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Optional<Gfx::Color> color, Optional<Gfx::Color> surface_clear_color = {}, Web::Painting::ContextRef context = {})
{
    ByteBuffer command_bytes;
    if (color.has_value()) {
        auto command = Web::Painting::FillRect { { 0, 0, 4, 4 }, *color, Gfx::CompositingAndBlendingOperator::Normal, Web::Painting::NO_EFFECT_NODE };
        append_display_list_command(command_bytes, command, command.rect, context);
    }
    return decode_display_list(visual_context_tree, move(command_bytes), surface_clear_color);
}

TEST_CASE(caret_damage_uses_the_sampled_visual_context_tree)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
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
    append_display_list_command(command_bytes, caret, caret.rect, { spatial });

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
    auto clip = builder.append_clip(Web::Painting::NO_CLIP_NODE, scroll_node, { 1, 2, 3, 4 });
    auto effect = builder.append_effects(Web::Painting::NO_EFFECT_NODE, scroll_node, clip, 0.5f);
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
    EXPECT_EQ(decoded_tree.node_count(), 4u);
    EXPECT_EQ(decoded_tree.live_node_count(), 4u);
    EXPECT_EQ(decoded_tree.effects_opacity(effect), Optional<float> { 0.5f });
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
            Web::Painting::CompositorScrollbar {
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto effect = builder.append_effects(Web::Painting::NO_EFFECT_NODE, spatial);
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
            .visual_context_node_indices = { effect.value() },
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
        make_display_list(visual_context_tree, Gfx::Color::Red, Gfx::Color::Transparent, { spatial, Web::Painting::NO_CLIP_NODE, effect }),
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
    auto const& updated_tree = context.sampled_visual_context_tree_for_testing();
    auto updated_translation = updated_tree.accumulated_matrix(spatial, {}, Web::Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes)[0, 3];
    EXPECT_EQ(updated_translation, 4.0f);
    EXPECT_EQ(updated_tree.effects_opacity(effect), Optional<float> { 0.5f });
    EXPECT(context.has_sampled_visual_animation_values_for_testing());

    context.install_display_list_update(
        make_display_list(visual_context_tree, Gfx::Color::Red, Gfx::Color::Transparent, { spatial, Web::Painting::NO_CLIP_NODE, effect }),
        visual_context_tree,
        {});
    EXPECT(!context.has_sampled_visual_animation_values_for_testing());
    auto const& replaced_tree = context.sampled_visual_context_tree_for_testing();
    auto replaced_translation = replaced_tree.accumulated_matrix(spatial, {}, Web::Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes)[0, 3];
    EXPECT_EQ(replaced_translation, 4.0f);
    EXPECT_EQ(replaced_tree.effects_opacity(effect), Optional<float> { 0.5f });
    EXPECT(context.has_sampled_visual_animation_values_for_testing());

    visual_context_tree.set_visual_animations(Vector<Web::Compositor::VisualAnimation> {});
    context.update_visual_context_tree(visual_context_tree, {});
    EXPECT(!context.has_sampled_visual_animation_values_for_testing());
    auto const& restored_tree = context.visual_context_tree_for_testing();
    auto restored_translation = restored_tree.accumulated_matrix(spatial, {}, Web::Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes)[0, 3];
    EXPECT_EQ(restored_translation, 0.0f);
    EXPECT_EQ(restored_tree.effects_opacity(effect), Optional<float> { 1.0f });
}

TEST_CASE(wheel_hit_testing_uses_the_current_visual_animation_tree)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
        make_scrollable_viewport_display_list(visual_context_tree, false, Web::Painting::ContextRef { animated_transform }),
        visual_context_tree,
        {});

    EXPECT(context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500)));
    auto result = context.async_scroll_by(
        Web::UniqueNodeID { 1 },
        { 20, 20 },
        { 0, 10 },
        { 0, 0, 100, 100 },
        Web::WheelDeltaPrecision::Precise, Web::ScrollGesturePhase::None, Web::UIEvents::KeyModifier::Mod_None,
        Web::Compositor::AsyncScrollOperationTracking::No);
    EXPECT(result.enqueue_result.accepted);
}

TEST_CASE(wheel_hit_testing_ignores_targets_from_a_larger_visual_context_tree)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto scroll_node = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    auto removed_transform = builder.append_transform(scroll_node, Gfx::FloatMatrix4x4::identity());
    auto visual_context_tree = builder.finish();

    context.install_display_list_update(
        make_scrollable_viewport_display_list(visual_context_tree, false, Web::Painting::ContextRef { removed_transform }),
        visual_context_tree,
        {});

    auto smaller_visual_context_tree = make_scrollable_viewport_visual_context_tree();
    context.install_display_list_update(
        make_scrollable_viewport_display_list(smaller_visual_context_tree, false, Web::Painting::ContextRef { removed_transform }),
        smaller_visual_context_tree,
        {});

    auto result = context.async_scroll_by(
        Web::UniqueNodeID { 1 },
        { 20, 20 },
        { 0, 10 },
        { 0, 0, 100, 100 },
        Web::WheelDeltaPrecision::Precise, Web::ScrollGesturePhase::None, Web::UIEvents::KeyModifier::Mod_None,
        Web::Compositor::AsyncScrollOperationTracking::No);
    EXPECT(result.enqueue_result.accepted);
}

TEST_CASE(pinch_zoom_copies_the_visual_context_tree_once_per_update)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Web::Compositor::VisualAnimationTransformOperation initial_scale {
        Web::Compositor::VisualAnimationTransformOperationKind::Scale,
        { 0 },
    };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto opacity_spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto opacity_effect = builder.append_effects(Web::Painting::NO_EFFECT_NODE, opacity_spatial, Web::Painting::NO_CLIP_NODE, 0);
    auto scale_spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, initial_scale.to_matrix());
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    visual_context_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Opacity,
            .visual_context_node_indices = { opacity_effect.value() },
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
    auto opacity_command = Web::Painting::FillRect { { 0, 0, 4, 4 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, Web::Painting::NO_EFFECT_NODE };
    append_display_list_command(command_bytes, opacity_command, opacity_command.rect, { opacity_spatial, Web::Painting::NO_CLIP_NODE, opacity_effect });
    auto scale_command = Web::Painting::FillRect { { 8, 0, 4, 4 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, Web::Painting::NO_EFFECT_NODE };
    append_display_list_command(command_bytes, scale_command, scale_command.rect, { scale_spatial });

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

TEST_CASE(background_color_animation_replaces_the_recorded_fill_color)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto effect = builder.append_background_color_animation(Web::Painting::NO_EFFECT_NODE, spatial);
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    visual_context_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::BackgroundColor,
            .visual_context_node_indices = { effect.value() },
            .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
            .iteration_duration_ms = 1000,
            .iteration_count = 1,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationValue { Gfx::Color::Red } },
                { 1, {}, Web::Compositor::VisualAnimationValue { Gfx::Color::Blue } },
            },
        },
    });

    ByteBuffer command_bytes;
    auto command = Web::Painting::FillRect {
        { 0, 0, 4, 4 },
        Gfx::Color::Green,
        Gfx::CompositingAndBlendingOperator::Normal,
        effect,
    };
    append_display_list_command(command_bytes, command, command.rect, { spatial, Web::Painting::NO_CLIP_NODE, effect });

    Gfx::IntRect viewport_rect { 0, 0, 4, 4 };
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
    EXPECT_EQ(bitmap->get_pixel(0, 0), (Gfx::Color { 128, 0, 128 }));
}

TEST_CASE(filter_animations_replace_their_effect_filters)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    Vector<Web::Painting::EffectNodeIndex> effects;
    Vector<Web::Compositor::VisualAnimation> animations;
    for (u32 i = 0; i < 8; ++i) {
        auto effect = builder.append_effects(Web::Painting::NO_EFFECT_NODE, spatial);
        effects.append(effect);
        animations.append({
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Filter,
            .visual_context_node_indices = { effect.value() },
            .iteration_duration_ms = 1000,
            .iteration_count = 1,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationFilterList { { .kind = Web::Compositor::VisualAnimationFilterOperationKind::Color, .amount = 1, .color_operation = Gfx::ColorFilterType::Opacity } } },
                { 1, {}, Web::Compositor::VisualAnimationFilterList { { .kind = Web::Compositor::VisualAnimationFilterOperationKind::Color, .amount = 0, .color_operation = Gfx::ColorFilterType::Opacity } } },
            },
        });
    }
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    for (auto& animation : animations)
        animation.monotonic_time_at_anchor_ns = anchor.nanoseconds();
    visual_context_tree.set_visual_animations(move(animations));

    ByteBuffer command_bytes;
    for (u32 i = 0; i < effects.size(); ++i) {
        auto command = Web::Painting::FillRect {
            { static_cast<i32>(i), 0, 1, 1 },
            Gfx::Color::Red,
            Gfx::CompositingAndBlendingOperator::Normal,
            Web::Painting::NO_EFFECT_NODE,
        };
        append_display_list_command(command_bytes, command, command.rect, { spatial, Web::Painting::NO_CLIP_NODE, effects[i] });
    }

    Gfx::IntRect viewport_rect { 0, 0, static_cast<i32>(effects.size()), 1 };
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
    for (u32 i = 0; i < effects.size(); ++i) {
        auto pixel = bitmap->get_pixel(i, 0);
        EXPECT_EQ(pixel.red(), 128);
        EXPECT_EQ(pixel.alpha(), 128);
    }
}

TEST_CASE(finite_visual_animations_stop_after_their_terminal_sample)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
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
        make_display_list(visual_context_tree, Gfx::Color::Red, Gfx::Color::Transparent, { spatial }),
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

TEST_CASE(delayed_visual_animations_remain_dormant_until_active_start)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto visual_context_tree = builder.finish();
    auto anchor = MonotonicTime::now();
    Web::Compositor::VisualAnimation animation {
        .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
        .visual_context_node_indices = { spatial.value() },
        .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
        .start_delay_ms = 1000,
        .iteration_duration_ms = 1000000,
        .fill_mode = Web::Compositor::VisualAnimationFillMode::Backwards,
        .easing = {},
        .keyframes = {
            { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
            { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 20 } } } },
        },
    };
    visual_context_tree.set_visual_animations({ animation });

    context.install_display_list_update(
        make_display_list(visual_context_tree, Gfx::Color::Red, {}, { spatial }),
        visual_context_tree,
        {});

    EXPECT(!context.has_active_visual_animations());
    EXPECT(!context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500)));
    EXPECT(!context.has_sampled_visual_animation_values_for_testing());

    animation.local_time_at_anchor_ms = 1000;
    visual_context_tree.set_visual_animations({ animation });
    context.update_visual_context_tree(visual_context_tree, {});

    EXPECT(context.has_active_visual_animations());
    EXPECT(context.advance_visual_animations(anchor));
    EXPECT(context.has_sampled_visual_animation_values_for_testing());
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 1 }, 1, client, canvas_surface_registry, false };
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
    virtual void rendering_opportunity(Web::Compositor::CompositorContextId, i64, double) override { }
    virtual void async_scroll_updates(Web::Compositor::CompositorContextId, Web::Compositor::PendingAsyncScrollUpdates const& updates) override
    {
        events.append("async_scroll_updates"_string);
        pushed_updates.append(updates);
    }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }

    String event_sequence() const { return MUST(String::join(","sv, events)); }

    Vector<String> events;
    Vector<Web::Compositor::PendingAsyncScrollUpdates> pushed_updates;
};

TEST_CASE(pending_scroll_updates_go_ahead_of_a_rendering_update_request)
{
    RecordingWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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

TEST_CASE(losing_the_scrollbar_a_drag_holds_ends_its_user_scroll_gesture)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };

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
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, false };

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
        Web::Painting::FillRect command { fill.rect, fill.color, Gfx::CompositingAndBlendingOperator::Normal, Web::Painting::NO_EFFECT_NODE };
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
    return { Web::Painting::SpatialNodeIndex { index } };
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
    Optional<Web::UniqueNodeID> document_id_of_nested_scroller {};
};

struct NestedScrollbarScene {
    Web::Painting::AccumulatedVisualContextTree visual_context_tree;
    NonnullRefPtr<Web::Painting::DisplayList> display_list;
    Web::Painting::SpatialNodeIndex viewport_scroll_node_index;
    Web::Painting::SpatialNodeIndex nested_scroll_node_index;
};

static Web::UniqueNodeID const viewport_scroller_node_id { 2 };
static Web::UniqueNodeID const nested_scroller_node_id { 3 };

// A viewport that scrolls by 100 holds a 40x40 scroller at 10,10 that scrolls by 120. The scroller's vertical scrollbar
// is painted by the display list: its track is 46,10 4x40, enlarged 42,10 8x40, and its 10 long thumb travels 0.25 device
// pixels per scrolled pixel.
static NestedScrollbarScene make_nested_scrollbar_scene(NestedScrollbarSceneOptions options = {})
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node_index = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    if (options.gives_nested_scroller_a_later_scroll_node_index)
        builder.append_scroll(viewport_scroll_node_index);
    auto spatial_node_of_scroller = viewport_scroll_node_index;
    if (!options.translation_of_scroller.is_zero())
        spatial_node_of_scroller = builder.append_transform(viewport_scroll_node_index, Gfx::translation_matrix(Gfx::FloatVector3 { options.translation_of_scroller.x(), options.translation_of_scroller.y(), 0 }));
    Web::Painting::ContextRef context_of_scroller { spatial_node_of_scroller };
    if (options.clip_of_scroller.has_value())
        context_of_scroller.clip = builder.append_clip(Web::Painting::NO_CLIP_NODE, spatial_node_of_scroller, *options.clip_of_scroller);
    auto nested_scroll_node_index = builder.append_scroll(spatial_node_of_scroller);
    auto visual_context_tree = builder.finish();

    Web::UniqueNodeID document_id { 1 };
    auto document_id_of_nested_scroller = options.document_id_of_nested_scroller.value_or(document_id);
    ByteBuffer command_bytes;
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = viewport_scroller_node_id,
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
        });
    if (options.main_thread_wheel_event_region_in_viewport.has_value()) {
        append_display_list_command(
            command_bytes,
            Web::Painting::CompositorMainThreadWheelEventRegion { .rect = *options.main_thread_wheel_event_region_in_viewport },
            {},
            in_spatial_node(viewport_scroll_node_index.value()));
    }
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorWheelHitTestTarget {
            .document_id = document_id_of_nested_scroller,
            .target_scroll_node_index = nested_scroll_node_index,
            .rect = { 10, 10, 40, 40 },
        },
        {},
        context_of_scroller);
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorScrollNode {
            .document_id = document_id_of_nested_scroller,
            .scrollable_node_id = nested_scroller_node_id,
            .scroll_node_index = nested_scroll_node_index,
            .parent_scroll_node_index = viewport_scroll_node_index,
            .scrollport_rect = { 10, 10, 40, 40 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 120 },
            .scroll_node_kind = Web::Painting::CompositorScrollNodeKind::Element,
            .pseudo_element_type = 0,
            .is_viewport = false,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        },
        {},
        context_of_scroller);
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorWheelHitTestTarget {
            .document_id = document_id_of_nested_scroller,
            .target_scroll_node_index = nested_scroll_node_index,
            .rect = { 10, 10, 40, 140 },
        },
        {},
        in_spatial_node(nested_scroll_node_index.value()));

    auto append_target_covering_scrollbar = [&] {
        append_display_list_command(
            command_bytes,
            Web::Painting::CompositorWheelHitTestTarget {
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
        Web::Painting::CompositorScrollbar {
            .document_id = document_id_of_nested_scroller,
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
        Web::Painting::DisplayList::AsyncScrollingMetadata {
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
        Web::Painting::ScrollStateSnapshot scroll_state_snapshot;
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
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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
    EXPECT_EQ(press_result.scrollbar_dragged_by_compositor->scroller_stable_node_id.kind, Web::Compositor::AsyncScrollNodeKind::Element);
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
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
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

// Drives wheel events at chosen times through the nested scrollbar scene: the UI path through wheel(), the WebContent
// path through wheel_from_document(). (20,20) is over the nested scroller, (80,80) over the viewport.
struct LatchedWheelContextFixture {
    explicit LatchedWheelContextFixture(NestedScrollbarSceneOptions options = {})
        : scene(options)
    {
        scene.context.viewport_size_updated({ 100, 100 }, Web::Compositor::WindowResizingInProgress::No);
    }

    Compositor::ContextState::ContextUpdateResult wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::ScrollGesturePhase phase, i64 milliseconds_after_start, u32 modifiers = Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision precision = Web::WheelDeltaPrecision::Precise)
    {
        return scene.context.async_scroll_by(position, delta, precision, phase, modifiers, now + AK::Duration::from_milliseconds(milliseconds_after_start));
    }

    Compositor::ContextState::ContextUpdateResult mouse_wheel_tick(Gfx::FloatPoint position, Gfx::FloatPoint delta, i64 milliseconds_after_start, u32 modifiers = Web::UIEvents::KeyModifier::Mod_None)
    {
        return wheel(position, delta, Web::ScrollGesturePhase::None, milliseconds_after_start, modifiers, Web::WheelDeltaPrecision::Discrete);
    }

    Compositor::ContextState::AsyncScrollResult wheel_from_document(Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::ScrollGesturePhase phase, i64 milliseconds_after_start)
    {
        return scene.context.async_scroll_by(document_id, position, delta, { 0, 0, 100, 100 }, Web::WheelDeltaPrecision::Precise, phase, Web::UIEvents::KeyModifier::Mod_None, Web::Compositor::AsyncScrollOperationTracking::Yes, now + AK::Duration::from_milliseconds(milliseconds_after_start));
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

TEST_CASE(a_wheel_gesture_latches_the_scroller_its_first_step_hit)
{
    LatchedWheelContextFixture fixture;

    EXPECT(fixture.wheel({ 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 0).accepted);
    auto offsets = fixture.take_scroll_offsets();
    EXPECT_EQ(offsets.nested, (Gfx::FloatPoint { 0, 50 }));
    EXPECT(!offsets.viewport.has_value());

    // The viewport is under the cursor now, but the gesture stays with the nested scroller.
    EXPECT(fixture.wheel({ 80, 80 }, { 0, 30 }, Web::ScrollGesturePhase::Ongoing, 10).accepted);
    offsets = fixture.take_scroll_offsets();
    EXPECT_EQ(offsets.nested, (Gfx::FloatPoint { 0, 80 }));
    EXPECT(!offsets.viewport.has_value());
    EXPECT_EQ(fixture.latched_scroller_node_id(), nested_scroller_node_id);
}

TEST_CASE(a_latched_scroller_absorbs_the_gesture_at_its_edge)
{
    LatchedWheelContextFixture fixture;

    EXPECT(fixture.wheel({ 20, 20 }, { 0, 100 }, Web::ScrollGesturePhase::Ongoing, 0).accepted);
    EXPECT(fixture.wheel({ 20, 20 }, { 0, 100 }, Web::ScrollGesturePhase::Ongoing, 10).accepted);
    EXPECT_EQ(fixture.take_scroll_offsets().nested, (Gfx::FloatPoint { 0, 120 }));

    auto step_past_the_edge = fixture.wheel({ 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 20);
    EXPECT(step_past_the_edge.accepted);
    EXPECT(!step_past_the_edge.frame_to_present.has_value());
    auto offsets = fixture.take_scroll_offsets();
    EXPECT(!offsets.nested.has_value());
    EXPECT(!offsets.viewport.has_value());

    // Once the gesture ended, the next one is routed afresh, past the scroller at its edge.
    fixture.wheel({ 20, 20 }, { 0, 0 }, Web::ScrollGesturePhase::Ended, 30);
    fixture.expect_step_to_scroll_viewport_afresh({ 20, 20 }, Web::ScrollGesturePhase::Ongoing, 300);
}

TEST_CASE(mouse_wheel_ticks_within_the_settle_delay_continue_the_latched_gesture)
{
    LatchedWheelContextFixture fixture;

    EXPECT(fixture.mouse_wheel_tick({ 20, 20 }, { 0, 100 }, 0).accepted);
    EXPECT(fixture.mouse_wheel_tick({ 20, 20 }, { 0, 100 }, 100).accepted);
    EXPECT_EQ(fixture.take_scroll_offsets().nested, (Gfx::FloatPoint { 0, 120 }));

    fixture.expect_step_to_be_absorbed_by_latched_scroller({ 25, 25 }, Web::ScrollGesturePhase::None, 300, Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision::Discrete);
    fixture.expect_step_to_scroll_viewport_afresh({ 20, 20 }, Web::ScrollGesturePhase::None, 900, Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision::Discrete);
}

TEST_CASE(a_mouse_wheel_tick_far_from_where_the_gesture_started_starts_a_new_gesture)
{
    LatchedWheelContextFixture fixture;
    fixture.latch_gesture_to_nested_scroller_at_its_edge(Web::ScrollGesturePhase::None, Web::WheelDeltaPrecision::Discrete);

    fixture.expect_step_to_be_absorbed_by_latched_scroller({ 27, 20 }, Web::ScrollGesturePhase::None, 50, Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision::Discrete);
    // 14 device pixels from where the gesture started, though only 7 from its last tick.
    fixture.expect_step_to_scroll_viewport_afresh({ 34, 20 }, Web::ScrollGesturePhase::None, 100, Web::UIEvents::KeyModifier::Mod_None, Web::WheelDeltaPrecision::Discrete);
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

TEST_CASE(momentum_arriving_late_after_the_gesture_ended_starts_a_new_gesture)
{
    LatchedWheelContextFixture fixture;
    fixture.latch_gesture_to_nested_scroller_at_its_edge();
    fixture.wheel({ 20, 20 }, { 0, 0 }, Web::ScrollGesturePhase::Ended, 10);

    fixture.expect_step_to_scroll_viewport_afresh({ 20, 20 }, Web::ScrollGesturePhase::Momentum, 150);
}

TEST_CASE(an_ongoing_step_after_the_gesture_ended_starts_a_new_gesture)
{
    LatchedWheelContextFixture fixture;
    fixture.latch_gesture_to_nested_scroller_at_its_edge();
    fixture.wheel({ 20, 20 }, { 0, 0 }, Web::ScrollGesturePhase::Ended, 10);

    fixture.expect_step_to_scroll_viewport_afresh({ 20, 20 }, Web::ScrollGesturePhase::Ongoing, 20);
}

TEST_CASE(a_latch_expires_when_no_step_arrives_within_the_settle_delay)
{
    LatchedWheelContextFixture fixture;
    fixture.latch_gesture_to_nested_scroller_at_its_edge();

    fixture.expect_step_to_scroll_viewport_afresh({ 20, 20 }, Web::ScrollGesturePhase::Ongoing, 600);
}

TEST_CASE(a_latched_wheel_gesture_outlives_a_display_list_that_renumbers_its_scroll_node)
{
    LatchedWheelContextFixture fixture;

    EXPECT(fixture.wheel({ 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 0).accepted);
    fixture.take_scroll_offsets();

    fixture.scene.install({ .gives_nested_scroller_a_later_scroll_node_index = true });
    fixture.take_scroll_offsets();

    EXPECT(fixture.wheel({ 80, 80 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 20).accepted);
    auto offsets = fixture.take_scroll_offsets();
    EXPECT_EQ(offsets.nested, (Gfx::FloatPoint { 0, 100 }));
    EXPECT(!offsets.viewport.has_value());
}

TEST_CASE(a_latch_is_dropped_when_its_scroller_leaves_the_display_list)
{
    LatchedWheelContextFixture fixture;

    EXPECT(fixture.wheel({ 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 0).accepted);
    fixture.take_scroll_offsets();

    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    fixture.scene.context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree, false), visual_context_tree, {});
    fixture.take_scroll_offsets();

    EXPECT(fixture.wheel({ 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 20).accepted);
    auto latched_scroller = fixture.scene.context.latched_wheel_scroller_for_testing();
    EXPECT(latched_scroller.has_value());
    EXPECT_EQ(latched_scroller->kind, Web::Compositor::AsyncScrollNodeKind::Viewport);
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

TEST_CASE(a_latched_step_over_another_document_is_left_to_that_document)
{
    Web::UniqueNodeID const parent_document_id { 1 };
    Web::UniqueNodeID const nested_document_id { 7 };
    LatchedWheelContextFixture fixture({ .document_id_of_nested_scroller = nested_document_id });

    EXPECT(!fixture.wheel_from_document(parent_document_id, { 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 0).enqueue_result.accepted);
    EXPECT(!fixture.latched_scroller_node_id().has_value());
    EXPECT(fixture.wheel_from_document(nested_document_id, { 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 0).enqueue_result.accepted);
    EXPECT_EQ(fixture.latched_scroller_node_id(), nested_scroller_node_id);
    fixture.take_scroll_offsets();

    // The parent navigable reports every step first; the nested one routes the steps of its scroller.
    EXPECT(!fixture.wheel_from_document(parent_document_id, { 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 20).enqueue_result.accepted);
    EXPECT_EQ(fixture.latched_scroller_node_id(), nested_scroller_node_id);
    EXPECT(fixture.wheel_from_document(nested_document_id, { 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Ongoing, 20).enqueue_result.accepted);
    EXPECT_EQ(fixture.take_scroll_offsets().nested, (Gfx::FloatPoint { 0, 100 }));

    fixture.wheel_from_document(parent_document_id, { 20, 20 }, { 0, 0 }, Web::ScrollGesturePhase::Ended, 30);
    fixture.wheel_from_document(nested_document_id, { 20, 20 }, { 0, 0 }, Web::ScrollGesturePhase::Ended, 31);
    EXPECT_EQ(fixture.latched_scroller_node_id(), nested_scroller_node_id);

    EXPECT(fixture.wheel_from_document(nested_document_id, { 20, 20 }, { 0, 50 }, Web::ScrollGesturePhase::Momentum, 80).enqueue_result.accepted);
    EXPECT_EQ(fixture.take_scroll_offsets().nested, (Gfx::FloatPoint { 0, 120 }));
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
};

struct RasterizingContextFixture {
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context;
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    Gfx::IntRect viewport_rect;

    explicit RasterizingContextFixture(Gfx::IntSize viewport_size = test_viewport_rect.size())
        : context(Web::Compositor::CompositorContextId { 1 }, 1, client, canvas_surface_registry, false)
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

static Web::Compositor::VisualAnimation rotation_animation(Web::Painting::SpatialNodeIndex spatial, MonotonicTime anchor)
{
    return {
        .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
        .visual_context_node_indices = { spatial.value() },
        .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
        .iteration_duration_ms = 1000,
        .easing = {},
        .keyframes = {
            { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::Rotate, { 0 } } } },
            { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::Rotate, { 2 * AK::Pi<float> } } } },
        },
    };
}

TEST_CASE(offscreen_rotations_sleep_until_scroll_or_viewport_changes)
{
    RasterizingContextFixture fixture;
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto scroll = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    auto spatial = builder.append_transform(scroll, Gfx::FloatMatrix4x4::identity(), { 4, 104 });
    auto tree = builder.finish();
    auto anchor = MonotonicTime::now();
    tree.set_visual_animations({ rotation_animation(spatial, anchor) });
    fixture.context.install_display_list_update(
        make_fills_display_list(tree, { { { 2, 102, 4, 4 }, Gfx::Color::Red, { spatial } } }), tree, {});
    fixture.rasterize();
    EXPECT(fixture.context.has_active_visual_animations());
    EXPECT(!fixture.context.visual_animations_need_frame());
    EXPECT(!fixture.context.visual_animations_need_frame());

    fixture.context.update_scroll_state(scroll_state_snapshot_with_offset(scroll, { 0, -100 }), {});
    EXPECT(fixture.context.visual_animations_need_frame());
    EXPECT(fixture.context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(2250)));
    fixture.rasterize();
    EXPECT_EQ(fixture.pixel(3, 3), Gfx::Color::Red);
    auto matrix = fixture.context.sampled_visual_context_tree_for_testing().accumulated_matrix(
        spatial, {}, Web::Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes);
    EXPECT(fabsf(matrix[0, 0]) < 0.001f);
    EXPECT(fabsf(matrix[1, 0] - 1) < 0.001f);

    fixture.context.update_scroll_state({}, {});
    EXPECT(!fixture.context.visual_animations_need_frame());
    fixture.context.viewport_size_updated({ 16, 120 }, Web::Compositor::WindowResizingInProgress::No);
    EXPECT(fixture.context.visual_animations_need_frame());
    fixture.context.viewport_size_updated({ 16, 16 }, Web::Compositor::WindowResizingInProgress::No);
    EXPECT(!fixture.context.visual_animations_need_frame());

    fixture.context.install_display_list_update(
        make_fills_display_list(tree, { { { 2, 2, 4, 4 }, Gfx::Color::Red, { spatial } } }), tree, {});
    EXPECT(fixture.context.visual_animations_need_frame());
}

TEST_CASE(offscreen_opacity_animations_sleep_and_resume_at_the_current_phase)
{
    RasterizingContextFixture fixture;
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto scroll = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    auto effect = builder.append_effects(Web::Painting::NO_EFFECT_NODE, scroll);
    auto child_effect = builder.append_effects(effect, scroll);
    auto tree = builder.finish();
    auto anchor = MonotonicTime::now();
    tree.set_visual_animations({ {
        .target_kind = Web::Compositor::VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { effect.value() },
        .monotonic_time_at_anchor_ns = anchor.nanoseconds(),
        .iteration_duration_ms = 1000,
        .easing = {},
        .keyframes = { { 0, {}, 1.0f }, { 1, {}, 0.0f } },
    } });
    fixture.context.install_display_list_update(
        make_fills_display_list(tree, { { { 2, 102, 4, 4 }, Gfx::Color::Red, { scroll, Web::Painting::NO_CLIP_NODE, child_effect } }, { { 0, 0, 1, 1 }, Gfx::Color::Green } }), tree, {});
    fixture.rasterize();
    EXPECT(fixture.context.has_active_visual_animations());
    EXPECT(!fixture.context.visual_animations_need_frame());
    EXPECT(!fixture.context.visual_animations_need_frame());
    fixture.context.update_scroll_state(scroll_state_snapshot_with_offset(scroll, { 0, -100 }), {});
    EXPECT(fixture.context.visual_animations_need_frame());
    EXPECT(fixture.context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(2500)));
    fixture.rasterize();
    EXPECT_EQ(fixture.pixel(3, 3).alpha(), 128);
    fixture.context.update_scroll_state({}, {});
    EXPECT(!fixture.context.visual_animations_need_frame());
    fixture.context.viewport_size_updated({ 16, 120 }, Web::Compositor::WindowResizingInProgress::No);
    EXPECT(fixture.context.visual_animations_need_frame());
}

TEST_CASE(finished_animations_do_not_keep_offscreen_animations_awake)
{
    RasterizingContextFixture fixture;
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto effect = builder.append_effects(Web::Painting::NO_EFFECT_NODE, spatial);
    auto tree = builder.finish();
    auto finished = rotation_animation(spatial, MonotonicTime::now());
    finished.iteration_count = 1;
    finished.local_time_at_anchor_ms = 2000;
    finished.keyframes = {
        { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateY, { 0 } } } },
        { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateY, { -100 } } } },
    };
    tree.set_visual_animations({ finished, {
                                               .target_kind = Web::Compositor::VisualAnimation::TargetKind::Opacity,
                                               .visual_context_node_indices = { effect.value() },
                                               .monotonic_time_at_anchor_ns = MonotonicTime::now().nanoseconds(),
                                               .iteration_duration_ms = 1000,
                                               .easing = {},
                                               .keyframes = { { 0, {}, 1.0f }, { 1, {}, 0.5f } },
                                           } });
    fixture.context.install_display_list_update(
        make_fills_display_list(tree, { { { 2, 202, 4, 4 }, Gfx::Color::Red, { spatial, Web::Painting::NO_CLIP_NODE, effect } } }), tree, {});
    fixture.rasterize();
    EXPECT(fixture.context.has_active_visual_animations());
    EXPECT(!fixture.context.visual_animations_need_frame());
    fixture.context.install_display_list_update(
        make_fills_display_list(tree, { { { 2, 102, 4, 4 }, Gfx::Color::Red, { spatial, Web::Painting::NO_CLIP_NODE, effect } } }), tree, {});
    EXPECT(fixture.context.visual_animations_need_frame());
    Vector<Web::Compositor::VisualAnimation> animations;
    for (auto const& animation : tree.visual_animations())
        animations.append(animation);
    auto anchor = MonotonicTime::now();
    animations[0].monotonic_time_at_anchor_ns = anchor.nanoseconds();
    animations[0].local_time_at_anchor_ms = 500;
    tree.set_visual_animations(move(animations));
    fixture.context.install_display_list_update(
        make_fills_display_list(tree, { { { 2, 202, 4, 4 }, Gfx::Color::Red, { spatial, Web::Painting::NO_CLIP_NODE, effect } } }), tree, {});
    fixture.context.advance_visual_animations(anchor);
    EXPECT(fixture.context.visual_animations_need_frame());
    fixture.context.advance_visual_animations(anchor + AK::Duration::from_milliseconds(500));
    EXPECT(!fixture.context.visual_animations_need_frame());
}

TEST_CASE(rotation_bounds_include_angles_that_can_reveal_offscreen_content)
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity());
    auto tree = builder.finish();
    auto display_list = make_fills_display_list(tree, { { { 50, 0, 4, 4 }, Gfx::Color::Red, { spatial } } });
    Array rotation_nodes { spatial };
    EXPECT(Web::Painting::animated_content_may_affect_viewport(
        display_list->command_bytes(), tree, {}, rotation_nodes, {}, { 0, 50, 10, 10 }));
    EXPECT(!Web::Painting::animated_content_may_affect_viewport(
        display_list->command_bytes(), tree, {}, rotation_nodes, {}, { 0, 100, 10, 10 }));
}

TEST_CASE(rotation_bounds_preserve_work_for_unbounded_commands_and_animated_clips)
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Gfx::FloatMatrix4x4::identity(), { 4, 104 });
    auto clip = builder.append_clip(Web::Painting::NO_CLIP_NODE, spatial, { 0, 0, 8, 8 });
    auto tree = builder.finish();
    Array rotation_nodes { spatial };
    auto unbounded = make_fills_display_list(tree, { { { 2, 102, 4, 4 }, Gfx::Color::Red, { spatial }, false } });
    EXPECT(Web::Painting::animated_content_may_affect_viewport(
        unbounded->command_bytes(), tree, {}, rotation_nodes, {}, test_viewport_rect));
    auto clipped = make_fills_display_list(tree, { { { 0, 0, 8, 8 }, Gfx::Color::Red, { Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, clip } } });
    EXPECT(Web::Painting::animated_content_may_affect_viewport(
        clipped->command_bytes(), tree, {}, rotation_nodes, {}, test_viewport_rect));
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
    fixture.expect_no_frame();

    canvas_surface_registry.set_canvas_surface(canvas_id, make_canvas_surface());
    EXPECT_EQ(fixture.present().damage_rect, (Gfx::IntRect { 3, 3, 6, 6 }));
    fixture.expect_no_frame();
}

TEST_CASE(compositor_initiated_presents_request_full_damage)
{
    PresentingContextFixture fixture { { 100, 100 }, true };
    auto visual_context_tree = make_scrollable_viewport_visual_context_tree();
    fixture.install(make_scrollable_viewport_display_list(visual_context_tree), visual_context_tree);
    fixture.present();

    auto already_presented = fixture.compositor_client.presented_frames.size();
    EXPECT(fixture.compositor_state->handle_mouse_event(fixture.context_id, mouse_event(Web::MouseEvent::Type::MouseMove, 98, 10)).handled);
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
    Web::Painting::FillRect nested_content { { 10, 10, 40, 10 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, Web::Painting::NO_EFFECT_NODE };
    append_display_list_command(command_bytes, nested_content, nested_content.rect, in_spatial_node(nested_scroll_node_index.value()));
    Web::Painting::FillRect viewport_content { { 60, 60, 10, 10 }, Gfx::Color::Green, Gfx::CompositingAndBlendingOperator::Normal, Web::Painting::NO_EFFECT_NODE };
    append_display_list_command(command_bytes, viewport_content, viewport_content.rect, in_spatial_node(viewport_scroll_node_index.value()));
    fixture.install(decode_display_list(visual_context_tree, move(command_bytes), {}, Web::Painting::DisplayList::AsyncScrollingMetadata { .viewport_rect = { 0, 0, 100, 100 } }), visual_context_tree);
    fixture.present();

    auto already_presented = fixture.compositor_client.presented_frames.size();
    EXPECT(fixture.compositor_state->async_scroll_by(fixture.context_id, { 20, 20 }, { 0, 5 }, Web::WheelDeltaPrecision::Precise, Web::ScrollGesturePhase::None, Web::UIEvents::KeyModifier::Mod_None));
    auto nested_scroll_frame = fixture.wait_for_frame(already_presented);
    EXPECT_EQ(nested_scroll_frame.content_rect, fixture.viewport_rect);
    EXPECT_EQ(nested_scroll_frame.damage_rect, (Gfx::IntRect { 9, 4, 42, 17 }));

    already_presented = fixture.compositor_client.presented_frames.size();
    EXPECT(fixture.compositor_state->async_scroll_by(fixture.context_id, { 80, 80 }, { 0, 10 }, Web::WheelDeltaPrecision::Precise, Web::ScrollGesturePhase::None, Web::UIEvents::KeyModifier::Mod_None));
    auto viewport_scroll_frame = fixture.wait_for_frame(already_presented);
    EXPECT_EQ(viewport_scroll_frame.content_rect, (Gfx::IntRect { 0, 10, 100, 100 }));
    EXPECT_EQ(viewport_scroll_frame.damage_rect, fixture.viewport_rect);
}

TEST_CASE(clip_paths_round_trip_through_serialized_tree_bytes)
{
    Gfx::Path star;
    star.move_to({ 65, 0 });
    star.line_to({ 35, 80 });
    star.line_to({ 105, 30 });
    star.line_to({ 25, 30 });
    star.line_to({ 95, 80 });
    star.close();
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto clip_path = builder.append_clip_path(Web::Painting::NO_CLIP_NODE, Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, star, { 25, 0, 80, 80 }, Gfx::WindingRule::EvenOdd);
    auto visual_context_tree = builder.finish();

    auto serialized_bytes = visual_context_tree.serialize_to_bytes();
    auto decoded_tree = MUST(Web::Painting::AccumulatedVisualContextTree::from_serialized_bytes(serialized_bytes));
    EXPECT_EQ(decoded_tree.serialize_to_bytes(), serialized_bytes);

    Web::Painting::ScrollStateSnapshot unscrolled;
    Web::Painting::ContextRef clip_path_context { Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, clip_path };
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
    auto effect = builder.append_effects(Web::Painting::NO_EFFECT_NODE, spatial, Web::Painting::NO_CLIP_NODE, 0.75f);
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
            .visual_context_node_indices = { effect.value() },
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
    EXPECT_EQ(sampled_tree.effects_opacity(effect), Optional<float> { 0.5f });
    EXPECT_EQ(sampled_tree.visual_animations().size(), 2u);

    auto source_translation = tree.accumulated_matrix(spatial, {}, include_viewport)[0, 3];
    EXPECT_EQ(source_translation, 12.0f);
    EXPECT_EQ(tree.effects_opacity(effect), Optional<float> { 0.75f });
}

// A scroll container that snaps along its y axis, with snap areas every 100 pixels.
static NonnullRefPtr<Web::Painting::DisplayList> make_snap_container_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Web::Painting::CompositorScrollNodeKind kind = Web::Painting::CompositorScrollNodeKind::Viewport)
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
            .max_scroll_offset = { 400, 400 },
            .scroll_node_kind = kind,
            .pseudo_element_type = 0,
            .is_viewport = kind == Web::Painting::CompositorScrollNodeKind::Viewport,
            .can_be_wheel_scrolled_horizontally = true,
            .can_be_wheel_scrolled_vertically = true,
        });
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorWheelHitTestTarget {
            .document_id = document_id,
            .target_scroll_node_index = scroll_node_index,
            .rect = { 0, 0, 100, 100 },
        });
    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorSnapContainer {
            .document_id = document_id,
            .scroll_node_index = scroll_node_index,
            .snapport = Web::CSSPixelRect { 0, 0, 100, 100 },
            .min_scroll_offset = Web::CSSPixelPoint { 0, 0 },
            .max_scroll_offset = Web::CSSPixelPoint { 400, 400 },
            .strictness = to_underlying(Web::CSS::ScrollSnapStrictness::Mandatory),
            .snaps_x = false,
            .snaps_y = true,
            .horizontal_writing_mode = true,
        });
    for (int i = 0; i < 5; ++i) {
        append_display_list_command(
            command_bytes,
            Web::Painting::CompositorSnapArea {
                .document_id = document_id,
                .scroll_node_index = scroll_node_index,
                .area_node_id = Web::UniqueNodeID { 10 + i },
                .pseudo_element_type = 0,
                .rect = Web::CSSPixelRect { 0, 100 * i, 100, 100 },
                .align_x = to_underlying(Web::CSS::ScrollSnapAlign::None),
                .align_y = to_underlying(Web::CSS::ScrollSnapAlign::Start),
                .always_stop = false,
            });
    }

    return decode_display_list(visual_context_tree, move(command_bytes), {},
        Web::Painting::DisplayList::AsyncScrollingMetadata {
            .viewport_rect = { 0, 0, 100, 100 },
            .device_pixels_per_css_pixel = 1.0,
        });
}

static constexpr Web::Compositor::AsyncScrollNodeStableID snap_container_stable_id { .node_id = Web::UniqueNodeID { 2 }, .kind = Web::Compositor::AsyncScrollNodeKind::Viewport, .pseudo_element_type = 0 };

struct SnapContainerContextFixture {
    RecordingWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { Web::Compositor::CompositorContextId { 0 }, 0, client, canvas_surface_registry, true };
    Web::Painting::AccumulatedVisualContextTree visual_context_tree { make_scrollable_viewport_visual_context_tree() };
    MonotonicTime now { MonotonicTime::now() };

    SnapContainerContextFixture(Web::Painting::CompositorScrollNodeKind kind = Web::Painting::CompositorScrollNodeKind::Viewport)
    {
        context.install_display_list_update(make_snap_container_display_list(visual_context_tree, kind), visual_context_tree, {});
    }

    Compositor::ContextState::AsyncScrollResult discrete_step(Gfx::FloatPoint delta, AK::Duration after = {})
    {
        return context.async_scroll_by(Web::UniqueNodeID { 1 }, { 50, 50 }, delta, { 0, 0, 100, 100 }, Web::WheelDeltaPrecision::Discrete, Web::ScrollGesturePhase::None, Web::UIEvents::KeyModifier::Mod_None, Web::Compositor::AsyncScrollOperationTracking::Yes, now + after);
    }

    // Everything the context published since the last call, merged the way WebContent merges it. The context pushes
    // its pending updates to WebContent whenever it asks for a rendering update, so what was pushed is read back
    // together with what is still pending.
    Web::Compositor::PendingAsyncScrollUpdates take_updates()
    {
        auto publications = move(client.pushed_updates);
        client.pushed_updates.clear();
        publications.append(context.take_pending_async_scroll_updates());

        Web::Compositor::PendingAsyncScrollUpdates merged;
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

    Web::Compositor::PendingAsyncScrollUpdates finish_animations(AK::Duration after)
    {
        (void)context.advance_smooth_scroll_animations(now + after);
        VERIFY(!context.has_active_smooth_scroll_animations());
        return take_updates();
    }
};

TEST_CASE(a_discrete_step_on_a_latched_snap_container_at_its_edge_is_consumed)
{
    SnapContainerContextFixture fixture;

    EXPECT(fixture.discrete_step({ 0, 500 }).enqueue_result.accepted);
    fixture.finish_animations(AK::Duration::from_milliseconds(1000));

    auto step_past_the_edge = fixture.discrete_step({ 0, 100 }, AK::Duration::from_milliseconds(50));
    EXPECT(step_past_the_edge.enqueue_result.accepted);
    EXPECT(step_past_the_edge.enqueue_result.operation_id.has_value());
    EXPECT(!fixture.context.has_active_smooth_scroll_animations());
    EXPECT(fixture.take_updates().completed_operation_ids.contains_slow(*step_past_the_edge.enqueue_result.operation_id));
    EXPECT(fixture.context.latched_wheel_scroller_for_testing().has_value());
}

TEST_CASE(a_discrete_wheel_step_on_a_snap_container_starts_a_snap_scroll)
{
    SnapContainerContextFixture fixture;

    auto result = fixture.discrete_step({ 0, 10 });
    EXPECT(result.enqueue_result.accepted);
    EXPECT(result.enqueue_result.operation_id.has_value());
    EXPECT(result.frame_to_present.has_value());
    EXPECT(fixture.context.has_active_smooth_scroll_animations());

    auto updates = fixture.take_updates();
    EXPECT_EQ(updates.document_id, Web::UniqueNodeID { 1 });
    EXPECT(updates.completed_operation_ids.is_empty());
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    auto started = updates.started_user_scrolls.first();
    EXPECT_EQ(started.stable_node_id, snap_container_stable_id);
    EXPECT_EQ(started.operation_id, *result.enqueue_result.operation_id);
    EXPECT_EQ(started.initial_scroll_offset, Web::CSSPixelPoint(0, 0));
    EXPECT_EQ(started.selection.position, Web::CSSPixelPoint(0, 100));
    EXPECT_EQ(started.unsnapped_scroll_destination, Web::CSSPixelPoint(0, 10));
    EXPECT(!started.selection.evaluated_x);
    EXPECT(started.selection.evaluated_y);
    EXPECT(started.selection.snapped_areas.x.is_empty());
    EXPECT_EQ(started.selection.snapped_areas.y.size(), 1u);
    EXPECT_EQ(started.selection.snapped_areas.y.first().node_id, Web::UniqueNodeID(11));

    updates = fixture.finish_animations(AK::Duration::from_seconds(1));
    EXPECT(updates.completed_operation_ids.contains_slow(started.operation_id));
    EXPECT(updates.started_user_scrolls.is_empty());
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(0, 100));
}

TEST_CASE(consecutive_discrete_wheel_steps_travel_from_the_offset_they_asked_for)
{
    SnapContainerContextFixture fixture;

    auto first_step = fixture.discrete_step({ 0, 10 });
    auto first_operation_id = *first_step.enqueue_result.operation_id;
    EXPECT_EQ(fixture.take_updates().started_user_scrolls.size(), 1u);

    // The second step asks for an offset short of the snap position the first is scrolling to, so it is consumed.
    auto second_step = fixture.discrete_step({ 0, 10 }, AK::Duration::from_milliseconds(10));
    EXPECT(second_step.enqueue_result.accepted);
    EXPECT(second_step.enqueue_result.operation_id.has_value());
    auto updates = fixture.take_updates();
    EXPECT(updates.started_user_scrolls.is_empty());
    EXPECT(updates.completed_operation_ids.contains_slow(*second_step.enqueue_result.operation_id));
    EXPECT(!updates.completed_operation_ids.contains_slow(first_operation_id));

    // The third step travels on from the 20 pixels the gesture has asked for, past the snap position in flight.
    auto third_step = fixture.discrete_step({ 0, 100 }, AK::Duration::from_milliseconds(20));
    updates = fixture.take_updates();
    EXPECT(updates.completed_operation_ids.contains_slow(first_operation_id));
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT_EQ(updates.started_user_scrolls.first().operation_id, *third_step.enqueue_result.operation_id);
    EXPECT_EQ(updates.started_user_scrolls.first().unsnapped_scroll_destination, Web::CSSPixelPoint(0, 120));
    EXPECT_EQ(updates.started_user_scrolls.first().selection.position, Web::CSSPixelPoint(0, 200));

    updates = fixture.finish_animations(AK::Duration::from_seconds(1));
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(0, 200));

    // A step arriving once the gesture has run out of input travels from where the scrolling box rests.
    fixture.discrete_step({ 0, 10 }, AK::Duration::from_seconds(2));
    updates = fixture.take_updates();
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT_EQ(updates.started_user_scrolls.first().initial_scroll_offset, Web::CSSPixelPoint(0, 200));
    EXPECT_EQ(updates.started_user_scrolls.first().selection.position, Web::CSSPixelPoint(0, 300));
}

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
    SnapContainerContextFixture fixture { Web::Painting::CompositorScrollNodeKind::Element };

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

TEST_CASE(a_discrete_wheel_step_along_a_non_snapping_axis_scrolls_by_its_delta)
{
    SnapContainerContextFixture fixture;

    auto result = fixture.discrete_step({ 10, 0 });
    EXPECT(result.enqueue_result.accepted);
    EXPECT(!fixture.context.has_active_smooth_scroll_animations());

    auto updates = fixture.take_updates();
    EXPECT(updates.started_user_scrolls.is_empty());
    EXPECT(updates.completed_operation_ids.contains_slow(*result.enqueue_result.operation_id));
    EXPECT_EQ(updates.scroll_offsets.size(), 1u);
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(10, 0));
}

TEST_CASE(a_scroll_by_the_main_thread_ends_the_wheel_gesture_on_the_compositor)
{
    SnapContainerContextFixture fixture;

    fixture.discrete_step({ 0, 10 });
    auto updates = fixture.finish_animations(AK::Duration::from_seconds(1));
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(0, 100));

    // The main thread adopted the snap scroll's offsets and then scrolled the box itself.
    auto scroll_state_snapshot = scroll_state_snapshot_with_offset(Web::Painting::SpatialNodeIndex { 1 }, { 0, -300 });
    scroll_state_snapshot.set_adopted_async_scroll_sequence(updates.sequence);
    fixture.context.update_scroll_state(move(scroll_state_snapshot), {});

    fixture.discrete_step({ 0, 10 }, AK::Duration::from_milliseconds(100));
    updates = fixture.take_updates();
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT_EQ(updates.started_user_scrolls.first().initial_scroll_offset, Web::CSSPixelPoint(0, 300));
    EXPECT_EQ(updates.started_user_scrolls.first().selection.position, Web::CSSPixelPoint(0, 400));
}

TEST_CASE(a_discrete_wheel_step_takes_over_a_smooth_scroll_the_main_thread_started)
{
    SnapContainerContextFixture fixture;

    auto first_step = fixture.discrete_step({ 0, 10 });
    auto first_operation_id = *first_step.enqueue_result.operation_id;
    fixture.take_updates();

    auto programmatic_scroll = fixture.context.smooth_scroll_to(snap_container_stable_id, { 0, 350 }, { 0, 0 }, { 0, 0, 100, 100 }, Web::Compositor::ScrollAnimationKind::SmoothScroll);
    EXPECT(programmatic_scroll.enqueue_result.accepted);
    auto programmatic_operation_id = *programmatic_scroll.enqueue_result.operation_id;
    auto updates = fixture.take_updates();
    EXPECT(updates.completed_operation_ids.contains_slow(first_operation_id));

    auto second_step = fixture.discrete_step({ 0, 10 }, AK::Duration::from_milliseconds(20));
    updates = fixture.take_updates();
    EXPECT(updates.operation_ids_taken_over_by_user_input.contains_slow(programmatic_operation_id));
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT_EQ(updates.started_user_scrolls.first().operation_id, *second_step.enqueue_result.operation_id);
    EXPECT_EQ(updates.started_user_scrolls.first().initial_scroll_offset, Web::CSSPixelPoint(0, 0));
    EXPECT_EQ(updates.started_user_scrolls.first().selection.position, Web::CSSPixelPoint(0, 100));
}

TEST_CASE(started_user_scrolls_round_trip_through_ipc)
{
    Web::Compositor::PendingAsyncScrollUpdates updates;
    updates.sequence = 7;
    updates.started_user_scrolls.append({
        .stable_node_id = snap_container_stable_id,
        .operation_id = 3,
        .initial_scroll_offset = { Web::CSSPixels(0), Web::CSSPixels(12.5) },
        .unsnapped_scroll_destination = { 0, 22 },
        .selection = {
            .position = { 0, 100 },
            .snapped_x = false,
            .snapped_y = true,
            .evaluated_x = false,
            .evaluated_y = true,
            .snapped_areas = { .x = {}, .y = { { .node_id = Web::UniqueNodeID { 11 }, .pseudo_element_type = 3 } } },
        },
        .settles_gesture = true,
    });

    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(updates));

    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    auto decoded = MUST(decoder.decode<Web::Compositor::PendingAsyncScrollUpdates>());

    EXPECT_EQ(decoded.sequence, 7u);
    EXPECT_EQ(decoded.started_user_scrolls.size(), 1u);
    auto const& started = decoded.started_user_scrolls.first();
    EXPECT_EQ(started.stable_node_id, snap_container_stable_id);
    EXPECT_EQ(started.operation_id, 3u);
    EXPECT_EQ(started.initial_scroll_offset, Web::CSSPixelPoint(Web::CSSPixels(0), Web::CSSPixels(12.5)));
    EXPECT_EQ(started.selection.position, Web::CSSPixelPoint(0, 100));
    EXPECT_EQ(started.unsnapped_scroll_destination, Web::CSSPixelPoint(0, 22));
    EXPECT(started.settles_gesture);
    EXPECT(!started.selection.snapped_x);
    EXPECT(started.selection.snapped_y);
    EXPECT(!started.selection.evaluated_x);
    EXPECT(started.selection.evaluated_y);
    EXPECT_EQ(started.selection.snapped_areas.y.size(), 1u);
    EXPECT_EQ(started.selection.snapped_areas.y.first().node_id, Web::UniqueNodeID(11));
    EXPECT_EQ(started.selection.snapped_areas.y.first().pseudo_element_type, 3u);
}

TEST_CASE(a_precise_pan_snaps_when_its_gesture_ends)
{
    SnapContainerContextFixture fixture;
    auto pan = [&](Gfx::FloatPoint delta, Web::ScrollGesturePhase phase, AK::Duration after) {
        return fixture.context.async_scroll_by(Web::UniqueNodeID { 1 }, { 50, 50 }, delta, { 0, 0, 100, 100 }, Web::WheelDeltaPrecision::Precise, phase, Web::UIEvents::KeyModifier::Mod_None, Web::Compositor::AsyncScrollOperationTracking::Yes, fixture.now + after);
    };

    // The finger pans the box to where it is released, past a snap position on the way.
    EXPECT(pan({ 0, 130 }, Web::ScrollGesturePhase::Ongoing, {}).enqueue_result.accepted);
    auto updates = fixture.take_updates();
    EXPECT(updates.started_user_scrolls.is_empty());
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(0, 130));

    auto gesture_end = pan({ 0, 0 }, Web::ScrollGesturePhase::Ended, AK::Duration::from_milliseconds(10));
    EXPECT(gesture_end.enqueue_result.accepted);
    EXPECT(gesture_end.enqueue_result.operation_id.has_value());
    EXPECT(fixture.context.has_active_smooth_scroll_animations());
    updates = fixture.take_updates();
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    auto started = updates.started_user_scrolls.first();
    EXPECT_EQ(started.operation_id, *gesture_end.enqueue_result.operation_id);
    EXPECT(started.settles_gesture);
    EXPECT_EQ(started.initial_scroll_offset, Web::CSSPixelPoint(0, 130));
    EXPECT_EQ(started.selection.position, Web::CSSPixelPoint(0, 100));

    // The gesture has ended, so ending it again snaps nothing.
    EXPECT(!pan({ 0, 0 }, Web::ScrollGesturePhase::Ended, AK::Duration::from_milliseconds(20)).enqueue_result.accepted);
}

TEST_CASE(the_momentum_of_a_flick_selects_a_snap_position_once)
{
    SnapContainerContextFixture fixture;
    auto flick = [&](Gfx::FloatPoint delta, Web::ScrollGesturePhase phase, AK::Duration after) {
        return fixture.context.async_scroll_by(Web::UniqueNodeID { 1 }, { 50, 50 }, delta, { 0, 0, 100, 100 }, Web::WheelDeltaPrecision::Precise, phase, Web::UIEvents::KeyModifier::Mod_None, Web::Compositor::AsyncScrollOperationTracking::Yes, fixture.now + after);
    };

    EXPECT(flick({ 0, 50 }, Web::ScrollGesturePhase::Ongoing, {}).enqueue_result.accepted);
    // The first momentum delta says nothing about where the momentum is headed, so it scrolls by its delta.
    EXPECT(flick({ 0, 100 }, Web::ScrollGesturePhase::Momentum, AK::Duration::from_milliseconds(10)).enqueue_result.accepted);
    auto updates = fixture.take_updates();
    EXPECT(updates.started_user_scrolls.is_empty());
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(0, 150));

    // The second decays by 0.6, so the momentum has 150 pixels left to travel, and snaps where that is headed.
    auto selecting_delta = flick({ 0, 60 }, Web::ScrollGesturePhase::Momentum, AK::Duration::from_milliseconds(20));
    EXPECT(selecting_delta.enqueue_result.accepted);
    updates = fixture.take_updates();
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    auto started = updates.started_user_scrolls.first();
    EXPECT_EQ(started.operation_id, *selecting_delta.enqueue_result.operation_id);
    EXPECT(!started.settles_gesture);
    EXPECT_EQ(started.initial_scroll_offset, Web::CSSPixelPoint(0, 150));
    EXPECT_EQ(started.selection.position, Web::CSSPixelPoint(0, 300));

    // The rest of the momentum is consumed by the scroll under way, and the end of the gesture leaves it be.
    auto consumed_delta = flick({ 0, 30 }, Web::ScrollGesturePhase::Momentum, AK::Duration::from_milliseconds(30));
    EXPECT(consumed_delta.enqueue_result.accepted);
    updates = fixture.take_updates();
    EXPECT(updates.started_user_scrolls.is_empty());
    EXPECT(updates.scroll_offsets.is_empty());
    EXPECT(updates.completed_operation_ids.contains_slow(*consumed_delta.enqueue_result.operation_id));

    EXPECT(!flick({ 0, 0 }, Web::ScrollGesturePhase::Ended, AK::Duration::from_milliseconds(40)).enqueue_result.accepted);
    EXPECT(fixture.context.has_active_smooth_scroll_animations());

    updates = fixture.finish_animations(AK::Duration::from_seconds(10));
    EXPECT(updates.completed_operation_ids.contains_slow(started.operation_id));
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(0, 300));
}

TEST_CASE(momentum_that_never_decays_snaps_from_where_the_gesture_started_at_its_end)
{
    SnapContainerContextFixture fixture;
    auto flick = [&](Gfx::FloatPoint delta, Web::ScrollGesturePhase phase, AK::Duration after) {
        return fixture.context.async_scroll_by(Web::UniqueNodeID { 1 }, { 50, 50 }, delta, { 0, 0, 100, 100 }, Web::WheelDeltaPrecision::Precise, phase, Web::UIEvents::KeyModifier::Mod_None, Web::Compositor::AsyncScrollOperationTracking::Yes, fixture.now + after);
    };

    // Momentum that keeps gathering pace says nothing about where it is headed, so every delta scrolls by itself.
    EXPECT(flick({ 0, 50 }, Web::ScrollGesturePhase::Ongoing, {}).enqueue_result.accepted);
    EXPECT(flick({ 0, 50 }, Web::ScrollGesturePhase::Momentum, AK::Duration::from_milliseconds(10)).enqueue_result.accepted);
    EXPECT(flick({ 0, 50 }, Web::ScrollGesturePhase::Momentum, AK::Duration::from_milliseconds(20)).enqueue_result.accepted);
    EXPECT(flick({ 0, 60 }, Web::ScrollGesturePhase::Momentum, AK::Duration::from_milliseconds(30)).enqueue_result.accepted);
    auto updates = fixture.take_updates();
    EXPECT(updates.started_user_scrolls.is_empty());
    EXPECT_EQ(updates.scroll_offsets.first().compositor_scroll_offset, Gfx::FloatPoint(0, 210));

    EXPECT(flick({ 0, 0 }, Web::ScrollGesturePhase::Ended, AK::Duration::from_milliseconds(40)).enqueue_result.accepted);
    updates = fixture.take_updates();
    EXPECT_EQ(updates.started_user_scrolls.size(), 1u);
    EXPECT(updates.started_user_scrolls.first().settles_gesture);
    EXPECT_EQ(updates.started_user_scrolls.first().initial_scroll_offset, Web::CSSPixelPoint(0, 210));
    EXPECT_EQ(updates.started_user_scrolls.first().selection.position, Web::CSSPixelPoint(0, 200));
}
