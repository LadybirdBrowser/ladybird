/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Tuple.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibTest/TestCase.h>

static Compositing::AsyncScrollNodeID const viewport_node_id { .document_id = Compositing::UniqueNodeID { 1 }, .scroll_node_index = Compositing::SpatialNodeIndex { 1 } };
static Compositing::UniqueNodeID const viewport_document_node_id { 2 };

static Compositing::AsyncScrollingState make_scrolling_state_with_viewport_scroll_node(float max_scroll_offset_y)
{
    Compositing::AsyncScrollingState state;
    state.scroll_nodes.append({
        .node_id = viewport_node_id,
        .stable_node_id = { .node_id = viewport_document_node_id, .kind = Compositing::AsyncScrollNodeKind::Viewport, .pseudo_element_type = 0 },
        .parent_node_id = {},
        .scrollport_rect = { 0, 0, 800, 600 },
        .min_scroll_offset = { 0, 0 },
        .max_scroll_offset = { 0, max_scroll_offset_y },
        .is_viewport = true,
        .can_be_wheel_scrolled_horizontally = false,
        .can_be_wheel_scrolled_vertically = true,
    });
    return state;
}

static Compositing::AsyncScrollTree make_scroll_tree_with_viewport_scroll_node(float max_scroll_offset_y)
{
    Compositing::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(make_scrolling_state_with_viewport_scroll_node(max_scroll_offset_y));
    return scroll_tree;
}

static Compositing::AccumulatedVisualContextTree make_visual_context_tree()
{
    Compositing::VisualContextTreeTestBuilder builder;
    return builder.finish();
}

static NonnullRefPtr<Compositing::DisplayList> make_empty_display_list(Compositing::AccumulatedVisualContextTree const& visual_context_tree)
{
    return Compositing::DisplayList::create_from_command_bytes(visual_context_tree, ByteBuffer {}, {});
}

TEST_CASE(wheel_hit_testing_rejects_a_different_visual_context_tree_structural_epoch)
{
    auto visual_context_tree = make_visual_context_tree();
    Compositing::AsyncScrollingState state;
    state.main_thread_wheel_event_regions.append({
        .context = { Compositing::VISUAL_VIEWPORT_NODE_INDEX },
        .rect = { 0, 0, 100, 100 },
    });

    Compositing::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    scroll_tree.rebuild_wheel_hit_test_targets(make_empty_display_list(visual_context_tree), &visual_context_tree, {});
    auto current_result = scroll_tree.hit_test_scroll_node_for_wheel(
        visual_context_tree, { 20, 20 }, { 0, 10 });
    EXPECT(current_result.blocked_by_main_thread_region);

    auto replacement_tree = make_visual_context_tree();
    auto replacement_result = scroll_tree.hit_test_scroll_node_for_wheel(
        replacement_tree, { 20, 20 }, { 0, 10 });
    EXPECT(!replacement_result.blocked_by_main_thread_region);
}

TEST_CASE(wheel_hit_testing_ignores_invalid_visual_context_indices)
{
    auto visual_context_tree = make_visual_context_tree();
    Compositing::AsyncScrollingState state;
    state.main_thread_wheel_event_regions.append({
        .context = { Compositing::SpatialNodeIndex { 100 } },
        .rect = { 0, 0, 100, 100 },
    });

    Compositing::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    scroll_tree.rebuild_wheel_hit_test_targets(make_empty_display_list(visual_context_tree), &visual_context_tree, {});
    auto result = scroll_tree.hit_test_scroll_node_for_wheel(
        visual_context_tree, { 20, 20 }, { 0, 10 });
    EXPECT(!result.blocked_by_main_thread_region);
}

TEST_CASE(blocking_wheel_event_hit_testing_fails_closed_for_invalid_visual_context_indices)
{
    auto visual_context_tree = make_visual_context_tree();
    auto state = make_scrolling_state_with_viewport_scroll_node(2000);
    state.has_blocking_wheel_event_listeners = true;
    state.blocking_wheel_event_regions.append({
        .context = { Compositing::SpatialNodeIndex { 100 } },
        .rect = { 0, 0, 100, 100 },
    });

    auto display_list = make_empty_display_list(visual_context_tree);
    EXPECT(Compositing::blocks_wheel_event_at_position(state, display_list, &visual_context_tree, {}, { 200, 200 }));
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, display_list, &visual_context_tree, {}, { 200, 200 }, { 0, 10 }, true),
        Compositing::WheelScrollAdmission::BlockedByWheelEventRegion);

    Compositing::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    scroll_tree.rebuild_wheel_hit_test_targets(display_list, &visual_context_tree, {});
    auto result = scroll_tree.hit_test_scroll_node_for_wheel(visual_context_tree, { 200, 200 }, { 0, 10 });
    EXPECT(result.blocked_by_wheel_event_region);
    EXPECT(!result.node_id.has_value());
}

TEST_CASE(wheel_scroll_admission_preserves_main_thread_region_and_viewport_blocker_precedence)
{
    auto visual_context_tree = make_visual_context_tree();
    auto display_list = make_empty_display_list(visual_context_tree);
    auto state = make_scrolling_state_with_viewport_scroll_node(2000);
    state.has_blocking_wheel_event_listeners = true;
    state.blocking_wheel_event_regions.append({
        .context = { Compositing::VISUAL_VIEWPORT_NODE_INDEX },
        .rect = { 0, 0, 100, 100 },
    });
    state.main_thread_wheel_event_regions.append({
        .context = { Compositing::VISUAL_VIEWPORT_NODE_INDEX },
        .rect = { 0, 0, 100, 100 },
    });

    EXPECT(Compositing::blocks_wheel_event_at_position(state, display_list, &visual_context_tree, {}, { 20, 20 }));
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, display_list, &visual_context_tree, {}, { 20, 20 }, { 0, 10 }, true),
        Compositing::WheelScrollAdmission::BlockedByMainThreadRegion);
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, display_list, &visual_context_tree, {}, { 20, 20 }, { 0, 10 }, false),
        Compositing::WheelScrollAdmission::BlockedByMainThreadRegion);

    state.has_blocking_wheel_event_region_covering_viewport = true;
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, display_list, &visual_context_tree, {}, { 20, 20 }, { 0, 10 }, true),
        Compositing::WheelScrollAdmission::BlockedByWheelEventRegion);
}

TEST_CASE(wheel_scroll_admission_fails_closed_for_stale_or_missing_blocker_geometry)
{
    Compositing::VisualContextTreeTestBuilder builder;
    builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    auto visual_context_tree = builder.finish();
    auto display_list = make_empty_display_list(visual_context_tree);
    auto state = make_scrolling_state_with_viewport_scroll_node(2000);
    state.has_blocking_wheel_event_listeners = true;
    state.blocking_wheel_event_regions.append({
        .context = { Compositing::VISUAL_VIEWPORT_NODE_INDEX },
        .rect = { 0, 0, 100, 100 },
    });

    EXPECT_EQ(Compositing::admit_wheel_scroll(state, display_list, &visual_context_tree, {}, { 200, 200 }, { 0, 10 }, true),
        Compositing::WheelScrollAdmission::Accepted);
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, display_list, &visual_context_tree, {}, { 200, 200 }, { 0, 10 }, false),
        Compositing::WheelScrollAdmission::StaleBlockingWheelEventRegions);
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, {}, &visual_context_tree, {}, { 200, 200 }, { 0, 10 }, true),
        Compositing::WheelScrollAdmission::BlockedByWheelEventRegion);
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, display_list, nullptr, {}, { 200, 200 }, { 0, 10 }, true),
        Compositing::WheelScrollAdmission::BlockedByWheelEventRegion);
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, {}, nullptr, {}, { 200, 200 }, { 0, 10 }, false),
        Compositing::WheelScrollAdmission::StaleBlockingWheelEventRegions);

    state.has_blocking_wheel_event_listeners = false;
    state.blocking_wheel_event_regions.clear();
    EXPECT_EQ(Compositing::admit_wheel_scroll(state, {}, nullptr, {}, { 200, 200 }, { 0, 10 }, true),
        Compositing::WheelScrollAdmission::NoScrollableTarget);
}

TEST_CASE(async_scrolling_resolves_sticky_offsets_from_the_visual_context_tree)
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node = builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
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
    Compositing::ScrollStateSnapshot snapshot;

    auto scroll_offset = scroll_tree.apply_scroll_delta(viewport_node_id, { 0, 300 }, visual_context_tree, snapshot, Compositing::ScrollChaining::ToScrollableAncestors);
    EXPECT(scroll_offset.has_value());
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

TEST_CASE(a_scroller_at_its_edge_hands_a_delta_to_its_ancestors_only_when_chaining_is_allowed)
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node = builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    VERIFY(viewport_scroll_node == viewport_node_id.scroll_node_index);
    auto nested_scroll_node = builder.append_scroll(viewport_scroll_node);
    auto visual_context_tree = builder.finish();

    Compositing::AsyncScrollNodeID const nested_scroll_node_id { .document_id = Compositing::UniqueNodeID { 1 }, .scroll_node_index = nested_scroll_node };
    auto make_scroll_tree_with_nested_scroller_that_scrolls_by_10 = [&] {
        auto state = make_scrolling_state_with_viewport_scroll_node(2000);
        state.scroll_nodes.append({
            .node_id = nested_scroll_node_id,
            .stable_node_id = { .node_id = Compositing::UniqueNodeID { 3 }, .kind = Compositing::AsyncScrollNodeKind::Element, .pseudo_element_type = 0 },
            .parent_node_id = viewport_node_id,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 10 },
            .is_viewport = false,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        });
        Compositing::AsyncScrollTree scroll_tree;
        scroll_tree.set_state(move(state));
        return scroll_tree;
    };

    auto scroll_nested_scroller_to_its_edge = [&](Compositing::AsyncScrollTree& scroll_tree, Compositing::ScrollStateSnapshot& snapshot) {
        auto scroll_offset = scroll_tree.apply_scroll_delta(nested_scroll_node_id, { 0, 10 }, visual_context_tree, snapshot, Compositing::ScrollChaining::None);
        EXPECT(scroll_offset.has_value());
        EXPECT_EQ(snapshot.device_offset_for_index(nested_scroll_node), (Gfx::FloatPoint { 0, -10 }));
    };

    {
        auto scroll_tree = make_scroll_tree_with_nested_scroller_that_scrolls_by_10();
        Compositing::ScrollStateSnapshot snapshot;
        scroll_nested_scroller_to_its_edge(scroll_tree, snapshot);

        auto scroll_offset = scroll_tree.apply_scroll_delta(nested_scroll_node_id, { 0, 30 }, visual_context_tree, snapshot, Compositing::ScrollChaining::ToScrollableAncestors);
        EXPECT(scroll_offset.has_value());
        EXPECT_EQ(scroll_offset->stable_node_id.node_id, viewport_document_node_id);
        EXPECT_EQ(snapshot.device_offset_for_index(viewport_scroll_node), (Gfx::FloatPoint { 0, -30 }));
        EXPECT_EQ(snapshot.device_offset_for_index(nested_scroll_node), (Gfx::FloatPoint { 0, -10 }));
    }

    {
        auto scroll_tree = make_scroll_tree_with_nested_scroller_that_scrolls_by_10();
        Compositing::ScrollStateSnapshot snapshot;
        scroll_nested_scroller_to_its_edge(scroll_tree, snapshot);

        auto scroll_offset = scroll_tree.apply_scroll_delta(nested_scroll_node_id, { 0, 30 }, visual_context_tree, snapshot, Compositing::ScrollChaining::None);
        EXPECT(!scroll_offset.has_value());
        EXPECT_EQ(snapshot.device_offset_for_index(viewport_scroll_node), (Gfx::FloatPoint { 0, 0 }));
        EXPECT_EQ(snapshot.device_offset_for_index(nested_scroll_node), (Gfx::FloatPoint { 0, -10 }));
    }

    {
        auto scroll_tree = make_scroll_tree_with_nested_scroller_that_scrolls_by_10();
        Compositing::ScrollStateSnapshot snapshot;

        // Once the child moves, the remainder of the delta does not scroll its ancestor.
        auto scroll_offset = scroll_tree.apply_scroll_delta(nested_scroll_node_id, { 0, 30 }, visual_context_tree, snapshot, Compositing::ScrollChaining::ToScrollableAncestors);
        EXPECT(scroll_offset.has_value());
        EXPECT_EQ(scroll_offset->stable_node_id.node_id, (Compositing::UniqueNodeID { 3 }));
        EXPECT_EQ(scroll_offset->unadopted_scroll_delta, (Gfx::FloatPoint { 0, 10 }));
        EXPECT_EQ(snapshot.device_offset_for_index(viewport_scroll_node), (Gfx::FloatPoint { 0, 0 }));
        EXPECT_EQ(snapshot.device_offset_for_index(nested_scroll_node), (Gfx::FloatPoint { 0, -10 }));
    }
}
