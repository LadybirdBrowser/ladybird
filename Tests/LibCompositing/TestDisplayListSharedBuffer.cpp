/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibTest/TestCase.h>
#include <Tests/LibCompositing/DisplayListTestHelpers.h>

using namespace Compositing;

static TestDisplayList two_fill_rects()
{
    TestDisplayList commands;
    ContextRef root { SpatialNodeIndex { 0 }, NO_CLIP_NODE, NO_EFFECT_NODE };
    append_display_list_command(commands, FillRect { { 0, 0, 10, 10 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal, NO_EFFECT_NODE }, Gfx::IntRect { 0, 0, 10, 10 }, root);
    append_display_list_command(commands, FillRect { { 5, 5, 10, 10 }, Gfx::Color::Blue, Gfx::CompositingAndBlendingOperator::Normal, NO_EFFECT_NODE }, Gfx::IntRect { 5, 5, 10, 10 }, root);
    return commands;
}

static NonnullRefPtr<DisplayList> list_from(AccumulatedVisualContextTree const& tree, TestDisplayList commands)
{
    return DisplayList::create_from_command_bytes(tree, move(commands.bytes), move(commands.runs));
}

TEST_CASE(a_display_list_round_trips_through_a_shared_buffer)
{
    auto tree = VisualContextTreeTestBuilder().finish();
    auto commands = two_fill_rects();
    auto sent = list_from(tree, commands);
    sent->set_surface_clear_color(Gfx::Color::White);

    auto buffer = MUST(sent->copy_to_shared_buffer());
    auto received = MUST(DisplayList::create_from_shared_buffer(sent->properties(), buffer, sent->command_bytes().size(), sent->command_runs().size()));
    EXPECT_EQ(received->id(), sent->id());
    EXPECT_EQ(received->surface_clear_color(), Optional<Gfx::Color> { Gfx::Color::White });
    EXPECT_EQ(received->command_bytes(), commands.bytes.bytes());
    EXPECT_EQ(received->command_runs().size(), commands.runs.size());
    EXPECT(received->command_runs()[0] == commands.runs[0]);

    // The tape is read from the mapping; the run table was copied out of it.
    EXPECT_EQ(received->command_bytes().data(), buffer.data<u8>());
    auto* shared_runs = buffer.data<u8>() + commands.bytes.size();
    for (size_t i = 0; i < commands.runs.size() * sizeof(DisplayListCommandRun); ++i)
        shared_runs[i] = 0xff;
    EXPECT(received->command_runs()[0] == commands.runs[0]);
}

TEST_CASE(an_empty_display_list_needs_no_buffer)
{
    auto tree = VisualContextTreeTestBuilder().finish();
    auto empty = DisplayList::create(tree);
    auto buffer = MUST(empty->copy_to_shared_buffer());
    EXPECT(!buffer.is_valid());
    auto received = MUST(DisplayList::create_from_shared_buffer(empty->properties(), buffer, 0, 0));
    EXPECT(received->command_bytes().is_empty());
    EXPECT(received->command_runs().is_empty());
}

TEST_CASE(sizes_that_do_not_fit_the_buffer_are_rejected)
{
    auto tree = VisualContextTreeTestBuilder().finish();
    auto commands = two_fill_rects();
    auto sent = list_from(tree, commands);
    auto buffer = MUST(sent->copy_to_shared_buffer());
    auto tape_size = sent->command_bytes().size();
    auto run_count = sent->command_runs().size();

    EXPECT(DisplayList::create_from_shared_buffer(sent->properties(), buffer, tape_size + 8, run_count).is_error());
    EXPECT(DisplayList::create_from_shared_buffer(sent->properties(), buffer, tape_size - 4, run_count).is_error());
    EXPECT(DisplayList::create_from_shared_buffer(sent->properties(), buffer, tape_size, run_count + 1).is_error());
    EXPECT(DisplayList::create_from_shared_buffer(sent->properties(), buffer, tape_size, NumericLimits<u64>::max()).is_error());
    EXPECT(DisplayList::create_from_shared_buffer(sent->properties(), Core::AnonymousBuffer {}, tape_size, run_count).is_error());
    // The runs in the buffer no longer cover the tape once the tape is claimed shorter than the last run.
    EXPECT(DisplayList::create_from_shared_buffer(sent->properties(), buffer, tape_size - 8, run_count).is_error());
}
