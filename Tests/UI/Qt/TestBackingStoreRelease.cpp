/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/BackingStoreRelease.h>

TEST_CASE(view_finished_before_compositor)
{
    size_t releases = 0;
    auto hold = make_ref_counted<Ladybird::BackingStoreRelease>([&] { ++releases; });
    hold->view_finished_reading();
    hold->view_finished_reading();
    EXPECT_EQ(releases, 0u);
    hold->compositor_finished_reading();
    EXPECT_EQ(releases, 1u);
    hold->compositor_finished_reading();
    hold->view_finished_reading();
    EXPECT_EQ(releases, 1u);
}

TEST_CASE(compositor_finished_before_view)
{
    size_t releases = 0;
    auto hold = make_ref_counted<Ladybird::BackingStoreRelease>([&] { ++releases; });
    hold->compositor_finished_reading();
    EXPECT_EQ(releases, 0u);
    hold->view_finished_reading();
    EXPECT_EQ(releases, 1u);
}

TEST_CASE(retired_buffer_outlives_view)
{
    size_t releases = 0;
    RefPtr<Ladybird::BackingStoreRelease> view_hold = make_ref_counted<Ladybird::BackingStoreRelease>([&] { ++releases; });
    Function<void()> on_buffer_release = [buffer_hold = view_hold] { buffer_hold->compositor_finished_reading(); };
    view_hold->view_finished_reading();
    view_hold = nullptr;
    EXPECT_EQ(releases, 0u);
    on_buffer_release();
    EXPECT_EQ(releases, 1u);
}

TEST_CASE(fallback_keeps_current_and_old_page_backup_until_view_finishes)
{
    size_t releases = 0;
    auto current = make_ref_counted<Ladybird::BackingStoreRelease>([&] { ++releases; });
    auto old_page_backup = make_ref_counted<Ladybird::BackingStoreRelease>([&] { ++releases; });
    current->compositor_finished_reading();
    old_page_backup->compositor_finished_reading();
    EXPECT_EQ(releases, 0u);
    current->view_finished_reading();
    EXPECT_EQ(releases, 1u);
    old_page_backup->view_finished_reading();
    EXPECT_EQ(releases, 2u);
}

TEST_CASE(completed_lease_does_not_consume_release_for_reused_backing_store)
{
    size_t releases = 0;
    auto old_lease = make_ref_counted<Ladybird::BackingStoreRelease>([&] { ++releases; });
    EXPECT(old_lease->view_finished_reading());
    old_lease->compositor_finished_reading();
    EXPECT_EQ(releases, 1u);
    EXPECT(old_lease->is_released());
    EXPECT(!old_lease->view_finished_reading());
    EXPECT_EQ(releases, 1u);
}
