/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/RecentLossCounter.h>

static constexpr auto decay_interval = AK::Duration::from_seconds(45);
static constexpr auto coalescing_window = AK::Duration::from_seconds(15);

static MonotonicTime at(i64 seconds)
{
    static auto const origin = MonotonicTime::now_coarse();
    return origin + AK::Duration::from_seconds(seconds);
}

// Each loss is of a helper launched just before it, after the losses before it.
static void record_loss_of_new_helper(WebView::RecentLossCounter& counter, MonotonicTime now)
{
    counter.record_loss(now, counter.current_epoch());
}

TEST_CASE(losses_of_helpers_launched_after_the_last_loss_count_however_soon_they_come)
{
    WebView::RecentLossCounter counter { 3, decay_interval, coalescing_window };
    EXPECT(!counter.is_at_maximum(at(0)));
    record_loss_of_new_helper(counter, at(0));
    record_loss_of_new_helper(counter, at(0));
    EXPECT(!counter.is_at_maximum(at(0)));
    record_loss_of_new_helper(counter, at(1));
    EXPECT(counter.is_at_maximum(at(1)));
}

TEST_CASE(losses_of_helpers_running_at_a_counted_loss_count_as_part_of_it_within_the_window)
{
    WebView::RecentLossCounter counter { 2, decay_interval, coalescing_window };
    auto epoch_of_running_helpers = counter.current_epoch();
    counter.record_loss(at(0), epoch_of_running_helpers);
    counter.record_loss(at(1), epoch_of_running_helpers);
    counter.record_loss(at(14), epoch_of_running_helpers);
    EXPECT(!counter.is_at_maximum(at(14)));
    counter.record_loss(at(15), epoch_of_running_helpers);
    EXPECT(counter.is_at_maximum(at(15)));
}

TEST_CASE(the_count_drops_by_one_every_decay_interval)
{
    WebView::RecentLossCounter counter { 3, decay_interval, coalescing_window };
    record_loss_of_new_helper(counter, at(0));
    record_loss_of_new_helper(counter, at(20));
    record_loss_of_new_helper(counter, at(40));
    EXPECT(counter.is_at_maximum(at(44)));
    EXPECT(!counter.is_at_maximum(at(45)));
    record_loss_of_new_helper(counter, at(60));
    EXPECT(counter.is_at_maximum(at(60)));
    // The count is down to one by then, so it takes two more losses to reach the maximum again.
    EXPECT(!counter.is_at_maximum(at(135)));
    record_loss_of_new_helper(counter, at(135));
    EXPECT(!counter.is_at_maximum(at(135)));
    record_loss_of_new_helper(counter, at(150));
    EXPECT(counter.is_at_maximum(at(150)));
}

TEST_CASE(a_loss_after_the_count_decays_away_waits_a_whole_interval)
{
    WebView::RecentLossCounter counter { 1, decay_interval, coalescing_window };
    record_loss_of_new_helper(counter, at(0));
    EXPECT(!counter.is_at_maximum(at(1000)));
    record_loss_of_new_helper(counter, at(1000));
    EXPECT(counter.is_at_maximum(at(1044)));
    EXPECT(!counter.is_at_maximum(at(1045)));
}
