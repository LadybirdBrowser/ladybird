/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibTest/TestCase.h>
#include <Tests/LibCompositing/DisplayListTestHelpers.h>

using namespace Compositing;

static ContextRef context(u32 spatial)
{
    return { SpatialNodeIndex { spatial }, NO_CLIP_NODE, NO_EFFECT_NODE };
}

static void append_fill_rect(TestDisplayList& commands, ContextRef context, Gfx::IntRect rect)
{
    append_display_list_command(commands, FillRect { rect, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, NO_EFFECT_NODE }, rect, context);
}

TEST_CASE(validation_rejects_gaps_overruns_and_misalignment)
{
    TestDisplayList commands;
    append_fill_rect(commands, context(0), { 0, 0, 10, 10 });
    append_fill_rect(commands, context(1), { 0, 0, 10, 10 });
    auto runs = commands.runs;
    EXPECT_EQ(runs.size(), 2u);
    EXPECT(!validate_display_list_command_runs(commands.bytes, runs).is_error());

    auto gap = runs;
    gap[1].offset += DisplayList::command_alignment;
    EXPECT(validate_display_list_command_runs(commands.bytes, gap).is_error());

    auto overrun = runs;
    overrun[1].size += DisplayList::command_alignment;
    EXPECT(validate_display_list_command_runs(commands.bytes, overrun).is_error());

    auto misaligned = runs;
    misaligned[0].size -= 1;
    misaligned[1].offset -= 1;
    misaligned[1].size += 1;
    EXPECT(validate_display_list_command_runs(commands.bytes, misaligned).is_error());

    EXPECT(validate_display_list_command_runs(commands.bytes, runs.span().slice(0, 1)).is_error());
    EXPECT(!validate_display_list_command_runs({}, {}).is_error());
}

#if DISPLAY_LIST_RUNS_DEBUG
TEST_CASE(validation_checks_run_boundaries_and_summaries)
{
    TestDisplayList commands;
    append_fill_rect(commands, context(0), { 0, 0, 10, 10 });
    append_fill_rect(commands, context(1), { 20, 20, 10, 10 });
    auto runs = commands.runs;

    auto split_command = runs;
    split_command[0].size -= DisplayList::command_alignment;
    split_command[1].offset -= DisplayList::command_alignment;
    split_command[1].size += DisplayList::command_alignment;
    EXPECT(validate_display_list_command_runs(commands.bytes, split_command).is_error());

    auto incorrect_bounds = runs;
    incorrect_bounds[0].ink_bounds = { 0, 0, 1, 1 };
    EXPECT(validate_display_list_command_runs(commands.bytes, incorrect_bounds).is_error());

    auto incorrect_metadata = runs;
    incorrect_metadata[0].has_compositor_metadata = true;
    EXPECT(validate_display_list_command_runs(commands.bytes, incorrect_metadata).is_error());

    auto incorrect_unbounded = runs;
    incorrect_unbounded[0].has_unbounded_draw = true;
    EXPECT(validate_display_list_command_runs(commands.bytes, incorrect_unbounded).is_error());

    auto adjacent_contexts = runs;
    adjacent_contexts[1].context = adjacent_contexts[0].context;
    EXPECT(validate_display_list_command_runs(commands.bytes, adjacent_contexts).is_error());
}
#endif
