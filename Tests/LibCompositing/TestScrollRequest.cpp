/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/Types.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

static RequestedRelativeScroll scroll_by(float delta, float minimum, float maximum)
{
    return {
        .delta = { 0, delta },
        .lower_bound = { 0, minimum },
        .upper_bound = { 0, maximum },
    };
}

// Applying each scroll in turn, clamped to the range it had.
static float scrolled_in_turn(float offset, ReadonlySpan<RequestedRelativeScroll> scrolls)
{
    for (auto const& scroll : scrolls)
        offset = scroll.applied_to({ 0, offset }).y();
    return offset;
}

TEST_CASE(folded_relative_scrolls_clamp_where_each_did)
{
    Array scrolls {
        scroll_by(300, 0, 500),
        scroll_by(-450, 0, 500),
        scroll_by(100, 0, 800),
    };
    auto folded = scrolls[0];
    folded.then(scrolls[1]);
    folded.then(scrolls[2]);

    for (float offset : { 0.0f, 100.0f, 250.0f, 400.0f, 500.0f, 700.0f })
        EXPECT_EQ(folded.applied_to({ 0, offset }).y(), scrolled_in_turn(offset, scrolls));
}

TEST_CASE(relative_scroll_after_a_jump_replaces_the_one_before)
{
    ScrollRequest request { .stable_node_id = {}, .request_id = 1, .jump_offset = {}, .visual_viewport_scale = {}, .relative_scroll = scroll_by(100, 0, 500), .relative_scroll_moves_visual_viewport_first = false, .instant_scroll_is_for_user_input = false, .smooth_scroll = {}, .takes_over_from_momentum = false };
    ScrollRequest jump { .stable_node_id = {}, .request_id = 2, .jump_offset = Gfx::FloatPoint { 0, 40 }, .visual_viewport_scale = {}, .relative_scroll = scroll_by(10, 0, 500), .relative_scroll_moves_visual_viewport_first = false, .instant_scroll_is_for_user_input = false, .smooth_scroll = {}, .takes_over_from_momentum = false };
    request.fold_later(jump);

    EXPECT_EQ(request.request_id, 2u);
    EXPECT_EQ(request.jump_offset, Gfx::FloatPoint(0, 40));
    EXPECT_EQ(request.relative_scroll->applied_to({ 0, 40 }).y(), 50.0f);
}

TEST_CASE(later_relative_scroll_folds_into_the_one_before)
{
    ScrollRequest request { .stable_node_id = {}, .request_id = 1, .jump_offset = Gfx::FloatPoint { 0, 40 }, .visual_viewport_scale = {}, .relative_scroll = scroll_by(500, 0, 500), .relative_scroll_moves_visual_viewport_first = false, .instant_scroll_is_for_user_input = false, .smooth_scroll = {}, .takes_over_from_momentum = false };
    ScrollRequest later { .stable_node_id = {}, .request_id = 2, .jump_offset = {}, .visual_viewport_scale = {}, .relative_scroll = scroll_by(-100, 0, 500), .relative_scroll_moves_visual_viewport_first = false, .instant_scroll_is_for_user_input = false, .smooth_scroll = {}, .takes_over_from_momentum = false };
    request.fold_later(later);

    EXPECT_EQ(request.jump_offset, Gfx::FloatPoint(0, 40));
    EXPECT_EQ(request.relative_scroll->applied_to({ 0, 40 }).y(), 400.0f);
}

static RequestedSmoothScroll smooth_scroll(Gfx::FloatPoint destination_offset, bool destination_is_relative, bool retargets_operation)
{
    return {
        .operation_id = first_operation_id_the_main_thread_requests + 1,
        .destination_offset = destination_offset,
        .destination_is_relative = destination_is_relative,
        .animation_kind = ScrollAnimationKind::SmoothScroll,
        .initiator = SmoothScrollInitiator::UserInput,
        .retargets_operation = retargets_operation,
        .settled_user_input_scroll_count = {},
    };
}

TEST_CASE(later_step_of_an_unsent_operation_goes_on_from_where_it_was_headed)
{
    auto by_a_distance = smooth_scroll({ 0, 100 }, true, false);
    by_a_distance.fold_later(smooth_scroll({ 0, 100 }, true, true));
    EXPECT_EQ(by_a_distance.destination_offset, Gfx::FloatPoint(0, 200));
    EXPECT(by_a_distance.destination_is_relative);
    EXPECT(by_a_distance.retargets_operation);

    auto to_an_offset = smooth_scroll({ 0, 300 }, false, false);
    to_an_offset.fold_later(smooth_scroll({ 0, 100 }, true, true));
    EXPECT_EQ(to_an_offset.destination_offset, Gfx::FloatPoint(0, 400));
    EXPECT(!to_an_offset.destination_is_relative);

    auto then_to_an_offset = smooth_scroll({ 0, 100 }, true, true);
    then_to_an_offset.fold_later(smooth_scroll({ 0, 50 }, false, false));
    EXPECT_EQ(then_to_an_offset.destination_offset, Gfx::FloatPoint(0, 50));
    EXPECT(!then_to_an_offset.destination_is_relative);
    EXPECT(then_to_an_offset.retargets_operation);
}
