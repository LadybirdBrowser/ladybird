/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ScrollbarController.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibTest/TestCase.h>

// A scroll shows the scrollbars, which only take a press while shown. Scrolling back leaves them where they started.
static void show_scrollbars_by_scrolling_away_and_back(Compositor::ScrollbarController& controller, Compositing::SpatialNodeIndex scroll_node_index)
{
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto now = MonotonicTime::now();
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, now);
    scroll_state_snapshot.set_device_offset_for_index(scroll_node_index, { 1, 1 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, now);
    scroll_state_snapshot.set_device_offset_for_index(scroll_node_index, { 0, 0 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, now);
}

static Compositor::ScrollbarController::Drag begin_scrollbar_drag(Gfx::Orientation orientation, Gfx::FloatPoint position, Optional<Gfx::IntRect> expanded_thumb_rect = {})
{
    auto vertical = orientation == Gfx::Orientation::Vertical;
    auto document_id = Web::UniqueNodeID { 1 };
    auto scroll_node_index = Compositing::SpatialNodeIndex { 1 };
    auto scroll_node_id = Compositing::AsyncScrollNodeID {
        .document_id = document_id,
        .scroll_node_index = scroll_node_index,
    };

    Compositing::AsyncScrollingState scrolling_state;
    scrolling_state.scroll_nodes.append({
        .node_id = scroll_node_id,
        .stable_node_id = {
            .node_id = Web::UniqueNodeID { 2 },
            .kind = Web::AsyncScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
        },
        .parent_node_id = {},
        .scrollport_rect = { 0, 0, 100, 100 },
        .min_scroll_offset = { 0, 0 },
        .max_scroll_offset = { 100, 100 },
        .is_viewport = true,
        .can_be_wheel_scrolled_horizontally = true,
        .can_be_wheel_scrolled_vertically = true,
    });

    Compositing::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(scrolling_state));
    Compositing::ScrollStateSnapshot scroll_state_snapshot;

    Vector<Compositing::AsyncScrollbar> scrollbars;
    scrollbars.append({
        .scroll_node_id = scroll_node_id,
        .scroller_stable_node_id = {},
        .scroll_node_index = scroll_node_index,
        .context = {},
        .paint_order_index = 0,
        .gutter_rect = vertical ? Gfx::IntRect { 96, 0, 4, 100 } : Gfx::IntRect { 0, 96, 100, 4 },
        .thumb_rect = vertical ? Gfx::IntRect { 98, 20, 2, 20 } : Gfx::IntRect { 20, 98, 20, 2 },
        .track_rect = vertical ? Gfx::IntRect { 96, 0, 4, 100 } : Gfx::IntRect { 0, 96, 100, 4 },
        .expanded_gutter_rect = vertical ? Gfx::IntRect { 92, 0, 8, 100 } : Gfx::IntRect { 0, 92, 100, 8 },
        .expanded_thumb_rect = expanded_thumb_rect.value_or(vertical ? Gfx::IntRect { 94, 20, 6, 20 } : Gfx::IntRect { 20, 94, 20, 6 }),
        .corner_clip_rect = {},
        .corner_clip_radius = 0,
        .scroll_size = 0.8,
        .expanded_scroll_size = 0.8,
        .min_scroll_offset = 0,
        .max_scroll_offset = 100,
        .thumb_color = Gfx::Color::Black,
        .track_color = Gfx::Color::Transparent,
        .vertical = vertical,
        .is_painted_by_compositor = true,
        .display_list_paints_enlarged_scrollbar = false,
    });

    Compositing::VisualContextTreeTestBuilder visual_context_tree_builder;
    visual_context_tree_builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    auto visual_context_tree = visual_context_tree_builder.finish();

    Compositor::ScrollbarController controller;
    controller.set_scrollbars(scrollbars);
    show_scrollbars_by_scrolling_away_and_back(controller, scroll_node_index);
    auto drag = controller.begin_drag(scroll_tree, visual_context_tree, scroll_state_snapshot, position);
    VERIFY(drag.has_value());
    return drag.release_value();
}

TEST_CASE(clicking_scrollbar_beside_thumb_grabs_thumb_at_that_position)
{
    auto vertical_drag = begin_scrollbar_drag(Gfx::Orientation::Vertical, { 97, 25 });
    EXPECT_EQ(vertical_drag.primary_position, 25);
    EXPECT_EQ(vertical_drag.thumb_grab_position, 5);

    auto horizontal_drag = begin_scrollbar_drag(Gfx::Orientation::Horizontal, { 25, 97 });
    EXPECT_EQ(horizontal_drag.primary_position, 25);
    EXPECT_EQ(horizontal_drag.thumb_grab_position, 5);
}

TEST_CASE(clicking_scrollbar_track_outside_thumb_grabs_thumb_at_center)
{
    auto vertical_drag = begin_scrollbar_drag(Gfx::Orientation::Vertical, { 97, 60 });
    EXPECT_EQ(vertical_drag.primary_position, 60);
    EXPECT_EQ(vertical_drag.thumb_grab_position, 10);

    auto horizontal_drag = begin_scrollbar_drag(Gfx::Orientation::Horizontal, { 60, 97 });
    EXPECT_EQ(horizontal_drag.primary_position, 60);
    EXPECT_EQ(horizontal_drag.thumb_grab_position, 10);
}

TEST_CASE(starting_scrollbar_drag_uses_expanded_thumb_geometry)
{
    auto vertical_drag = begin_scrollbar_drag(Gfx::Orientation::Vertical, { 97, 25 }, Gfx::IntRect { 94, 18, 6, 24 });
    EXPECT_EQ(vertical_drag.thumb_grab_position, 7);

    auto horizontal_drag = begin_scrollbar_drag(Gfx::Orientation::Horizontal, { 25, 97 }, Gfx::IntRect { 18, 94, 24, 6 });
    EXPECT_EQ(horizontal_drag.thumb_grab_position, 7);
}

static Compositing::AsyncScrollbar scrollbar_painted_by_display_list(bool display_list_paints_enlarged_scrollbar = false)
{
    return {
        .scroll_node_id = {
            .document_id = Web::UniqueNodeID { 1 },
            .scroll_node_index = Compositing::SpatialNodeIndex { 1 },
        },
        .scroller_stable_node_id = Web::AsyncScrollNodeStableID {
            .node_id = Web::UniqueNodeID { 2 },
            .kind = Web::AsyncScrollNodeKind::Element,
            .pseudo_element_type = 0,
        },
        .scroll_node_index = Compositing::SpatialNodeIndex { 1 },
        .context = {},
        .paint_order_index = 0,
        .gutter_rect = {},
        .thumb_rect = { 98, 20, 2, 20 },
        .track_rect = { 96, 0, 4, 100 },
        .expanded_gutter_rect = { 92, 0, 8, 100 },
        .expanded_thumb_rect = { 94, 20, 6, 20 },
        .corner_clip_rect = {},
        .corner_clip_radius = 0,
        .scroll_size = 0.8,
        .expanded_scroll_size = 0.8,
        .min_scroll_offset = 0,
        .max_scroll_offset = 100,
        .thumb_color = Gfx::Color::Black,
        .track_color = Gfx::Color::Transparent,
        .vertical = true,
        .is_painted_by_compositor = false,
        .display_list_paints_enlarged_scrollbar = display_list_paints_enlarged_scrollbar,
    };
}

static Compositor::ScrollbarController::PresentedScrollbar presented_scrollbar(Compositor::ScrollbarController const& controller, Compositing::ScrollStateSnapshot const& scroll_state_snapshot = {})
{
    auto scrollbar = controller.presented_scrollbar_painted_by_display_list(Compositing::SpatialNodeIndex { 1 }, true, Gfx::Color(0, 0, 0, 100), Gfx::Color(0, 0, 0, 20), scroll_state_snapshot);
    VERIFY(scrollbar.has_value());
    return *scrollbar;
}

static float opacity_of_scrollbar(Compositor::ScrollbarController const& controller)
{
    return presented_scrollbar(controller).opacity;
}

static auto const half_fade_duration = AK::Duration::from_milliseconds(Compositing::OverlayFade::duration.to_milliseconds() / 2);

static Compositing::AccumulatedVisualContextTree visual_context_tree_with_scroller()
{
    Compositing::VisualContextTreeTestBuilder visual_context_tree_builder;
    visual_context_tree_builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    return visual_context_tree_builder.finish();
}

TEST_CASE(scrollbar_is_hidden_until_its_scroller_scrolls)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto start = MonotonicTime::now();

    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    EXPECT_EQ(opacity_of_scrollbar(controller), 0);
    EXPECT(!controller.next_fade_out_time().has_value());

    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    EXPECT_EQ(opacity_of_scrollbar(controller), 1);
    EXPECT(controller.next_fade_out_time() == start + Compositor::ScrollbarController::shown_duration_after_activity);
}

TEST_CASE(scrollbar_fades_out_after_its_scroller_stops_scrolling)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto visual_context_tree = visual_context_tree_with_scroller();
    auto start = MonotonicTime::now();
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);

    auto shown_until = start + Compositor::ScrollbarController::shown_duration_after_activity;
    EXPECT(!controller.fades_need_frame(shown_until - AK::Duration::from_milliseconds(1)));
    EXPECT(controller.fades_need_frame(shown_until));

    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until);
    EXPECT_EQ(opacity_of_scrollbar(controller), 1);
    EXPECT(!controller.next_fade_out_time().has_value());

    auto damage_rect = controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until + half_fade_duration);
    EXPECT(opacity_of_scrollbar(controller) > 0);
    EXPECT(opacity_of_scrollbar(controller) < 1);
    EXPECT(damage_rect.contains(Gfx::IntRect { 92, 0, 8, 100 }));

    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until + Compositing::OverlayFade::duration);
    EXPECT_EQ(opacity_of_scrollbar(controller), 0);
    EXPECT(!controller.fades_need_frame(shown_until + Compositing::OverlayFade::duration));
}

TEST_CASE(scroll_while_fading_out_shows_scrollbar_again)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto visual_context_tree = visual_context_tree_with_scroller();
    auto start = MonotonicTime::now();
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);

    auto shown_until = start + Compositor::ScrollbarController::shown_duration_after_activity;
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until);
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until + half_fade_duration);
    EXPECT(opacity_of_scrollbar(controller) < 1);

    auto scrolled_again_at = shown_until + half_fade_duration;
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 20 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, scrolled_again_at);
    EXPECT_EQ(opacity_of_scrollbar(controller), 1);
    EXPECT(!controller.fades_need_frame(scrolled_again_at));
    EXPECT(controller.next_fade_out_time() == scrolled_again_at + Compositor::ScrollbarController::shown_duration_after_activity);
}

TEST_CASE(hovering_hidden_scrollbar_does_not_show_it)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto start = MonotonicTime::now();
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);

    controller.set_scrollbars({ scrollbar_painted_by_display_list(true) });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    EXPECT_EQ(opacity_of_scrollbar(controller), 0);
    EXPECT(!controller.fades_need_frame(start));

    // A scroll shows it, and the hover then keeps it shown and grows it.
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    EXPECT_EQ(opacity_of_scrollbar(controller), 1);
    EXPECT(!controller.next_fade_out_time().has_value());
    EXPECT(controller.fades_need_frame(start));
}

TEST_CASE(hovered_scrollbar_stays_shown_until_the_pointer_leaves_it)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto start = MonotonicTime::now();
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);

    controller.set_scrollbars({ scrollbar_painted_by_display_list(true) });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    EXPECT_EQ(opacity_of_scrollbar(controller), 1);
    EXPECT(!controller.next_fade_out_time().has_value());

    auto much_later = start + AK::Duration::from_seconds(10);
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, much_later);
    (void)controller.advance_fades(visual_context_tree_with_scroller(), scroll_state_snapshot, much_later);
    EXPECT_EQ(opacity_of_scrollbar(controller), 1);
    EXPECT(!controller.fades_need_frame(much_later));

    controller.set_scrollbars({ scrollbar_painted_by_display_list(false) });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, much_later);
    EXPECT_EQ(opacity_of_scrollbar(controller), 1);
    EXPECT(controller.next_fade_out_time() == much_later + Compositor::ScrollbarController::shown_duration_after_activity);
}

TEST_CASE(hovered_scrollbar_grows_and_stays_expanded_until_it_has_faded_out)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto visual_context_tree = visual_context_tree_with_scroller();
    auto start = MonotonicTime::now();
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);

    // A scroll shows the thin scrollbar.
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    EXPECT_EQ(presented_scrollbar(controller).thumb_rect.width(), 2);
    EXPECT_EQ(presented_scrollbar(controller).thumb_color.alpha(), 100);
    EXPECT_EQ(presented_scrollbar(controller).track_color.alpha(), 0);

    // Hovering it grows it over the duration of a fade.
    controller.set_scrollbars({ scrollbar_painted_by_display_list(true) });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    EXPECT(controller.fades_need_frame(start));
    auto damage_rect = controller.advance_fades(visual_context_tree, scroll_state_snapshot, start + half_fade_duration);
    EXPECT(damage_rect.contains(Gfx::IntRect { 92, 0, 8, 100 }));
    EXPECT(presented_scrollbar(controller).thumb_rect.width() > 2);
    EXPECT(presented_scrollbar(controller).thumb_rect.width() < 6);
    auto grown_at = start + Compositing::OverlayFade::duration;
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, grown_at);
    EXPECT_EQ(presented_scrollbar(controller, scroll_state_snapshot).thumb_rect, Gfx::FloatRect(94, 12, 6, 20));
    EXPECT_EQ(presented_scrollbar(controller).gutter_rect, Gfx::FloatRect(92, 0, 8, 100));
    EXPECT_EQ(presented_scrollbar(controller).thumb_color.alpha(), 100);
    EXPECT_EQ(presented_scrollbar(controller).track_color.alpha(), 20);
    EXPECT(!controller.fades_need_frame(grown_at));

    // It stays expanded after the pointer leaves it, while it is shown and while it fades out.
    controller.set_scrollbars({ scrollbar_painted_by_display_list(false) });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, grown_at);
    auto shown_until = grown_at + Compositor::ScrollbarController::shown_duration_after_activity;
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until);
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until + half_fade_duration);
    EXPECT(opacity_of_scrollbar(controller) > 0);
    EXPECT_EQ(presented_scrollbar(controller).thumb_rect.width(), 6);

    // Once it has faded out, it is thin again.
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until + Compositing::OverlayFade::duration);
    EXPECT_EQ(opacity_of_scrollbar(controller), 0);
    EXPECT_EQ(presented_scrollbar(controller).thumb_rect.width(), 2);
    EXPECT(!controller.fades_need_frame(shown_until + Compositing::OverlayFade::duration));
}

TEST_CASE(scrolling_expanded_scrollbar_the_display_list_paints_thin_damages_it)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto visual_context_tree = visual_context_tree_with_scroller();
    auto start = MonotonicTime::now();
    (void)controller.update_visibility(&visual_context_tree, scroll_state_snapshot, start);
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 5 });
    (void)controller.update_visibility(&visual_context_tree, scroll_state_snapshot, start);
    controller.set_scrollbars({ scrollbar_painted_by_display_list(true) });
    (void)controller.update_visibility(&visual_context_tree, scroll_state_snapshot, start);
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, start + Compositing::OverlayFade::duration);

    // While the display list paints the scrollbar expanded, it damages it on a scroll itself.
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    EXPECT(controller.update_visibility(&visual_context_tree, scroll_state_snapshot, start).is_empty());

    controller.set_scrollbars({ scrollbar_painted_by_display_list(false) });
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 20 });
    auto damage_rect = controller.update_visibility(&visual_context_tree, scroll_state_snapshot, start);
    EXPECT(damage_rect.contains(Gfx::IntRect { 92, 0, 8, 100 }));
}

TEST_CASE(press_beside_thumb_pages_toward_pointer_until_thumb_reaches_it)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });

    // The thumb spans 20 to 40 at no scroll offset, and moves 0.8 for each unit scrolled.
    EXPECT_EQ(controller.paging_direction(0, 10, { 0, 0 }), -1);
    EXPECT_EQ(controller.paging_direction(0, 30, { 0, 0 }), 0);
    EXPECT_EQ(controller.paging_direction(0, 60, { 0, 0 }), 1);
    EXPECT_EQ(controller.paging_direction(0, 60, { 0, 30 }), 0);
    EXPECT_EQ(controller.paging_direction(0, 60, { 0, 60 }), -1);
}

TEST_CASE(scrollbar_that_has_faded_out_is_reported_hidden)
{
    Compositor::ScrollbarController controller;
    controller.set_scrollbars({ scrollbar_painted_by_display_list() });
    Compositing::ScrollStateSnapshot scroll_state_snapshot;
    auto visual_context_tree = visual_context_tree_with_scroller();
    auto start = MonotonicTime::now();

    // A scrollbar starts out hidden, which the main thread hears once.
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    auto changes = controller.take_visibility_changes();
    EXPECT_EQ(changes.size(), 1u);
    EXPECT(changes[0].vertical);
    EXPECT(changes[0].hidden);
    EXPECT(controller.take_visibility_changes().is_empty());

    // A scroll shows it.
    scroll_state_snapshot.set_device_offset_for_index(Compositing::SpatialNodeIndex { 1 }, { 0, 10 });
    (void)controller.update_visibility(nullptr, scroll_state_snapshot, start);
    changes = controller.take_visibility_changes();
    EXPECT_EQ(changes.size(), 1u);
    EXPECT(!changes[0].hidden);

    // It is still shown while it fades out, and hidden once it has.
    auto shown_until = start + Compositor::ScrollbarController::shown_duration_after_activity;
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until);
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until + half_fade_duration);
    EXPECT(controller.take_visibility_changes().is_empty());
    (void)controller.advance_fades(visual_context_tree, scroll_state_snapshot, shown_until + Compositing::OverlayFade::duration);
    changes = controller.take_visibility_changes();
    EXPECT_EQ(changes.size(), 1u);
    EXPECT(changes[0].hidden);
}
