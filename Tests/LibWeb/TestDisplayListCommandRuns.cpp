/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Painting/DisplayList.h>
#include <Tests/LibWeb/DisplayListTestHelpers.h>

using namespace Web::Painting;

static ContextRef context(u32 spatial, Optional<u32> frame = {})
{
    return { SpatialNodeIndex { spatial }, frame.has_value() ? FrameNodeIndex { *frame } : NO_FRAME_NODE };
}

static void append_fill_rect(ByteBuffer& bytes, ContextRef context, Gfx::IntRect rect)
{
    append_display_list_command(bytes, FillRect { rect, Gfx::Color::Red }, rect, context);
}

static void append_clip_rect(ByteBuffer& bytes, ContextRef context, Gfx::IntRect rect)
{
    append_display_list_command(bytes, AddClipRect { rect.to_type<float>() }, rect, context, true);
}

TEST_CASE(runs_split_on_context_changes_and_cover_the_tape)
{
    ByteBuffer bytes;
    auto a = context(1);
    auto b = context(1, 0);
    append_fill_rect(bytes, a, { 0, 0, 10, 10 });
    append_fill_rect(bytes, a, { 20, 20, 10, 10 });
    append_fill_rect(bytes, b, { 5, 5, 10, 10 });
    append_fill_rect(bytes, a, { 0, 0, 1, 1 });

    auto runs = compute_display_list_command_runs(bytes);
    EXPECT(!validate_display_list_command_runs(bytes, runs).is_error());
    EXPECT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].context, a);
    EXPECT_EQ(runs[0].ink_bounds, Gfx::IntRect(0, 0, 30, 30));
    EXPECT(runs[0].is_self_contained);
    EXPECT(!runs[0].has_unbounded_draw);
    EXPECT(!runs[0].has_compositor_metadata);
    EXPECT_EQ(runs[1].context, b);
    EXPECT_EQ(runs[1].ink_bounds, Gfx::IntRect(5, 5, 10, 10));
    EXPECT_EQ(runs[2].context, a);
}

TEST_CASE(ink_bounds_skip_clips_and_metadata_while_balanced_saves_stay_self_contained)
{
    ByteBuffer bytes;
    auto root = context(0);
    append_display_list_command(bytes, Save {}, {}, root);
    append_clip_rect(bytes, root, { 0, 0, 500, 500 });
    append_fill_rect(bytes, root, { 5, 5, 10, 10 });
    append_display_list_command(bytes, Restore {}, {}, root);
    append_display_list_command(bytes, CompositorBlockingWheelEventRegion { Gfx::FloatRect { 0, 0, 900, 900 } }, {}, root);

    auto runs = compute_display_list_command_runs(bytes);
    EXPECT(!validate_display_list_command_runs(bytes, runs).is_error());
    EXPECT_EQ(runs.size(), 1u);
    auto const& run = runs[0];
    EXPECT_EQ(run.ink_bounds, Gfx::IntRect(5, 5, 10, 10));
    EXPECT(run.has_compositor_metadata);
    EXPECT(!run.has_unbounded_draw);
    EXPECT(!run.has_unconfined_clip);
    EXPECT_EQ(run.nesting_delta, 0);
    EXPECT_EQ(run.min_relative_nesting, 0);
    EXPECT(run.is_self_contained);
}

TEST_CASE(open_saves_and_unconfined_clips_are_not_self_contained)
{
    auto root = context(0);
    {
        ByteBuffer bytes;
        append_display_list_command(bytes, Save {}, {}, root);
        auto runs = compute_display_list_command_runs(bytes);
        EXPECT_EQ(runs[0].nesting_delta, 1);
        EXPECT_EQ(runs[0].min_relative_nesting, 0);
        EXPECT(!runs[0].is_self_contained);
    }
    {
        ByteBuffer bytes;
        append_display_list_command(bytes, Restore {}, {}, root);
        append_display_list_command(bytes, Save {}, {}, root);
        auto runs = compute_display_list_command_runs(bytes);
        EXPECT_EQ(runs[0].nesting_delta, 0);
        EXPECT_EQ(runs[0].min_relative_nesting, -1);
        EXPECT(!runs[0].is_self_contained);
    }
    {
        ByteBuffer bytes;
        append_clip_rect(bytes, root, { 0, 0, 10, 10 });
        append_fill_rect(bytes, root, { 0, 0, 10, 10 });
        auto runs = compute_display_list_command_runs(bytes);
        EXPECT(runs[0].has_unconfined_clip);
        EXPECT(!runs[0].is_self_contained);
    }
}

TEST_CASE(a_draw_without_bounds_marks_the_run_unbounded)
{
    ByteBuffer bytes;
    append_display_list_command(bytes, FillRect { { 0, 0, 10, 10 }, Gfx::Color::Red }, {}, context(0));

    auto runs = compute_display_list_command_runs(bytes);
    EXPECT(runs[0].has_unbounded_draw);
    EXPECT(runs[0].ink_bounds.is_empty());
    EXPECT(runs[0].is_self_contained);
}

TEST_CASE(validation_rejects_gaps_overruns_and_misalignment)
{
    ByteBuffer bytes;
    append_fill_rect(bytes, context(0), { 0, 0, 10, 10 });
    append_fill_rect(bytes, context(1), { 0, 0, 10, 10 });
    auto runs = compute_display_list_command_runs(bytes);
    EXPECT_EQ(runs.size(), 2u);
    EXPECT(!validate_display_list_command_runs(bytes, runs).is_error());

    auto gap = runs;
    gap[1].offset += DisplayList::command_alignment;
    EXPECT(validate_display_list_command_runs(bytes, gap).is_error());

    auto overrun = runs;
    overrun[1].size += DisplayList::command_alignment;
    EXPECT(validate_display_list_command_runs(bytes, overrun).is_error());

    auto misaligned = runs;
    misaligned[0].size -= 1;
    misaligned[1].offset -= 1;
    misaligned[1].size += 1;
    EXPECT(validate_display_list_command_runs(bytes, misaligned).is_error());

    EXPECT(validate_display_list_command_runs(bytes, runs.span().slice(0, 1)).is_error());
    EXPECT(!validate_display_list_command_runs({}, {}).is_error());
}
