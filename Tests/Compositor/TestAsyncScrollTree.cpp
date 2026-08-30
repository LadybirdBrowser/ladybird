/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Tuple.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/VisualContextTreeTestBuilder.h>

static Web::Compositor::AsyncScrollNodeID const viewport_node_id { .document_id = Web::UniqueNodeID { 1 }, .scroll_node_index = Web::Painting::SpatialNodeIndex { 1 } };

static Web::Compositor::AsyncScrollTree make_scroll_tree_with_viewport_scroll_node(float max_scroll_offset_y)
{
    Web::Compositor::AsyncScrollingState state;
    state.scroll_nodes.append({
        .node_id = viewport_node_id,
        .stable_node_id = { .node_id = Web::UniqueNodeID { 2 }, .kind = Web::Compositor::AsyncScrollNodeKind::Viewport, .pseudo_element_type = 0 },
        .parent_node_id = {},
        .scrollport_rect = { 0, 0, 800, 600 },
        .min_scroll_offset = { 0, 0 },
        .max_scroll_offset = { 0, max_scroll_offset_y },
        .is_viewport = true,
        .can_be_wheel_scrolled_horizontally = false,
        .can_be_wheel_scrolled_vertically = true,
    });
    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    return scroll_tree;
}

static Web::Painting::AccumulatedVisualContextTree make_visual_context_tree()
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    return builder.finish();
}

static NonnullRefPtr<Web::Painting::DisplayList> make_empty_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree)
{
    return Web::Painting::DisplayList::create_from_command_bytes(visual_context_tree, ByteBuffer {}, {});
}

TEST_CASE(wheel_hit_testing_rejects_a_different_visual_context_tree_version)
{
    auto visual_context_tree = make_visual_context_tree();
    Web::Compositor::AsyncScrollingState state;
    state.main_thread_wheel_event_regions.append({
        .context = { Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Web::Painting::NO_FRAME_NODE },
        .rect = { 0, 0, 100, 100 },
    });

    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    scroll_tree.rebuild_wheel_hit_test_targets(make_empty_display_list(visual_context_tree), &visual_context_tree, {});
    auto current_result = scroll_tree.hit_test_scroll_node_for_wheel(
        visual_context_tree, { 20, 20 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor);
    EXPECT(current_result.blocked_by_main_thread_region);

    auto replacement_tree = make_visual_context_tree();
    auto replacement_result = scroll_tree.hit_test_scroll_node_for_wheel(
        replacement_tree, { 20, 20 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor);
    EXPECT(!replacement_result.blocked_by_main_thread_region);
}

TEST_CASE(wheel_hit_testing_ignores_invalid_visual_context_indices)
{
    auto visual_context_tree = make_visual_context_tree();
    Web::Compositor::AsyncScrollingState state;
    state.main_thread_wheel_event_regions.append({
        .context = { Web::Painting::SpatialNodeIndex { 100 }, Web::Painting::NO_FRAME_NODE },
        .rect = { 0, 0, 100, 100 },
    });

    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    scroll_tree.rebuild_wheel_hit_test_targets(make_empty_display_list(visual_context_tree), &visual_context_tree, {});
    auto result = scroll_tree.hit_test_scroll_node_for_wheel(
        visual_context_tree, { 20, 20 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor);
    EXPECT(!result.blocked_by_main_thread_region);
}

TEST_CASE(wheel_hit_testing_prefilters_static_targets_but_tracks_animated_targets)
{
    auto make_tree_and_spatial = [](Gfx::FloatMatrix4x4 const& matrix = Gfx::FloatMatrix4x4::identity(), Optional<u64> version = {}) {
        Web::Painting::VisualContextTreeTestBuilder builder;
        auto spatial = builder.append_transform(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, matrix);
        return Tuple { version.has_value() ? builder.finish_with_version(*version) : builder.finish(), spatial };
    };
    auto make_scroll_tree = [](Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Web::Painting::SpatialNodeIndex spatial) {
        Web::Compositor::AsyncScrollingState state;
        state.main_thread_wheel_event_regions.append({
            .context = { spatial, Web::Painting::NO_FRAME_NODE },
            .rect = { 0, 0, 10, 10 },
        });
        Web::Compositor::AsyncScrollTree scroll_tree;
        scroll_tree.set_state(move(state));
        scroll_tree.rebuild_wheel_hit_test_targets(make_empty_display_list(visual_context_tree), &visual_context_tree, {});
        return scroll_tree;
    };
    auto hit_test = [](Web::Compositor::AsyncScrollTree const& scroll_tree, Web::Painting::AccumulatedVisualContextTree const& visual_context_tree) {
        return scroll_tree.hit_test_scroll_node_for_wheel(
            visual_context_tree, { 15, 5 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor);
    };

    auto static_tree_and_spatial = make_tree_and_spatial();
    auto static_spatial = static_tree_and_spatial.get<1>();
    auto static_tree = move(static_tree_and_spatial.get<0>());
    auto static_scroll_tree = make_scroll_tree(static_tree, static_spatial);
    auto moved_matrix = Gfx::FloatMatrix4x4::identity();
    moved_matrix[0, 3] = 10;
    auto moved_static_tree = move(make_tree_and_spatial(moved_matrix, static_tree.version()).get<0>());
    EXPECT(!hit_test(static_scroll_tree, moved_static_tree).blocked_by_main_thread_region);

    auto animated_tree_and_spatial = make_tree_and_spatial();
    auto animated_spatial = animated_tree_and_spatial.get<1>();
    auto animated_tree = move(animated_tree_and_spatial.get<0>());
    animated_tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { animated_spatial.value() },
            .monotonic_time_at_anchor_ns = 0,
            .iteration_duration_ms = 100,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 20 } } } },
            },
        },
    });
    auto animated_scroll_tree = make_scroll_tree(animated_tree, animated_spatial);
    auto sampled_animated_tree = animated_tree.with_visual_animation_samples(50'000'000);
    EXPECT(hit_test(animated_scroll_tree, sampled_animated_tree).blocked_by_main_thread_region);
}

TEST_CASE(blocking_wheel_event_hit_testing_fails_closed_for_invalid_visual_context_indices)
{
    auto visual_context_tree = make_visual_context_tree();
    Web::Compositor::AsyncScrollingState state;
    state.has_blocking_wheel_event_listeners = true;
    state.blocking_wheel_event_regions.append({
        .context = { Web::Painting::SpatialNodeIndex { 100 }, Web::Painting::NO_FRAME_NODE },
        .rect = { 0, 0, 100, 100 },
    });

    EXPECT(Web::Compositor::blocks_wheel_event_at_position(
        state, make_empty_display_list(visual_context_tree), &visual_context_tree, {}, { 20, 20 }));
}

TEST_CASE(async_scrolling_resolves_sticky_offsets_from_the_visual_context_tree)
{
    Web::Painting::VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node = builder.append_scroll(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    // A 50px header 100px down the document, sticking to the top of the scrollport within a 2000px containing block.
    auto header_node = builder.append_sticky(viewport_scroll_node,
        {
            .scroller = viewport_scroll_node,
            .parent_sticky = {},
            .position_relative_to_scroller = { 0, 100 },
            .border_box_size = { 800, 50 },
            .scrollport_size = { 800, 600 },
            .containing_block_region = { 0, 0, 800, 2000 },
            .needs_parent_offset_adjustment = true,
            .inset_top = 0.f,
            .inset_right = {},
            .inset_bottom = {},
            .inset_left = {},
        });
    // A 10px bar inside the header, sticking 20px below the scrollport top within the header's box.
    auto bar_node = builder.append_sticky(header_node,
        {
            .scroller = viewport_scroll_node,
            .parent_sticky = header_node,
            .position_relative_to_scroller = { 0, 110 },
            .border_box_size = { 800, 10 },
            .scrollport_size = { 800, 600 },
            .containing_block_region = { 0, 100, 800, 50 },
            .needs_parent_offset_adjustment = true,
            .inset_top = 20.f,
            .inset_right = {},
            .inset_bottom = {},
            .inset_left = {},
        });
    auto visual_context_tree = builder.finish();

    auto scroll_tree = make_scroll_tree_with_viewport_scroll_node(2000);
    Web::Painting::ScrollStateSnapshot snapshot;

    auto scroll_offsets = scroll_tree.apply_scroll_delta(viewport_node_id, { 0, 300 }, visual_context_tree, snapshot);
    EXPECT_EQ(scroll_offsets.size(), 1u);
    EXPECT_EQ(snapshot.device_offset_for_index(viewport_scroll_node), (Gfx::FloatPoint { 0, -300 }));
    // The scrollport top passed the header by 200px, so the header follows it by that much.
    EXPECT_EQ(snapshot.device_offset_for_index(header_node), (Gfx::FloatPoint { 0, 200 }));
    // With the header at 300, the bar sits at 310 and its inset asks for 320.
    EXPECT_EQ(snapshot.device_offset_for_index(bar_node), (Gfx::FloatPoint { 0, 10 }));

    // Past the end of the containing block, the header pins to the block's bottom edge and the bar to the header's.
    EXPECT(scroll_tree.set_scroll_offset(viewport_node_id, { 0, 1960 }, visual_context_tree, snapshot).has_value());
    EXPECT_EQ(snapshot.device_offset_for_index(header_node), (Gfx::FloatPoint { 0, 1850 }));
    EXPECT_EQ(snapshot.device_offset_for_index(bar_node), (Gfx::FloatPoint { 0, 20 }));

    // Scrolled back above the header, nothing sticks.
    EXPECT(scroll_tree.set_scroll_offset(viewport_node_id, { 0, 50 }, visual_context_tree, snapshot).has_value());
    EXPECT_EQ(snapshot.device_offset_for_index(header_node), (Gfx::FloatPoint { 0, 0 }));
    EXPECT_EQ(snapshot.device_offset_for_index(bar_node), (Gfx::FloatPoint { 0, 0 }));
}
