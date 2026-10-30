/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <AudioServer/CaptureActivity.h>
#include <LibTest/TestCase.h>

static constexpr Audio::PlaybackGroupId GROUP { 7 };
static constexpr Audio::PlaybackGroupId OTHER_GROUP { 8 };

struct Report {
    u64 tab_id;
    Audio::PlaybackGroupId group;
    bool capturing;

    bool operator==(Report const&) const = default;
};

struct ActivityFixture {
    Vector<Report> reports;
    AudioServer::CaptureActivity activity;

    ActivityFixture()
        : activity([this](u64 tab_id, Audio::PlaybackGroupId group, bool capturing) {
            reports.append({ tab_id, group, capturing });
        })
    {
    }
};

TEST_CASE(the_first_stream_of_a_group_reports_capture_at_once)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, GROUP);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true } }));
}

TEST_CASE(the_group_captures_until_its_last_stream_is_gone)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_stopped(1, GROUP);
    EXPECT_EQ(fixture.reports.size(), 1u);

    fixture.activity.stream_stopped(1, GROUP);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true }, { 1, GROUP, false } }));
}

TEST_CASE(a_group_that_captures_again_is_reported_again)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_stopped(1, GROUP);
    fixture.activity.stream_started(1, GROUP);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true }, { 1, GROUP, false }, { 1, GROUP, true } }));
}

TEST_CASE(groups_and_tabs_capture_apart)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, OTHER_GROUP);
    fixture.activity.stream_started(2, GROUP);
    fixture.activity.stream_stopped(1, GROUP);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true }, { 1, OTHER_GROUP, true }, { 2, GROUP, true }, { 1, GROUP, false } }));
}

TEST_CASE(a_stop_without_a_start_is_ignored)
{
    ActivityFixture fixture;
    fixture.activity.stream_stopped(1, GROUP);
    EXPECT(fixture.reports.is_empty());
}

TEST_CASE(each_group_counts_its_own_streams)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, OTHER_GROUP);
    EXPECT_EQ(fixture.activity.stream_count_for_testing(1, GROUP), 2u);
    EXPECT_EQ(fixture.activity.stream_count_for_testing(1, OTHER_GROUP), 1u);
    EXPECT_EQ(fixture.activity.stream_count_for_testing(2, GROUP), 0u);

    fixture.activity.stream_stopped(1, GROUP);
    EXPECT_EQ(fixture.activity.stream_count_for_testing(1, GROUP), 1u);
}
