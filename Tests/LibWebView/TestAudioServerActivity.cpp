/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibWebView/AudioServerActivity.h>
#include <LibWebView/AudioServerControlClient.h>

static constexpr u64 TAB_ID = 1;
static constexpr Audio::PlaybackGroupId GROUP { 1 };
static constexpr Audio::PlaybackGroupId OTHER_GROUP { 2 };
static constexpr auto AUDIBLE = Audio::Audibility::Audible;
static constexpr auto INAUDIBLE = Audio::Audibility::Inaudible;

struct Change {
    Audio::PlaybackGroupId group;
    Audio::Audibility audibility;
    bool operator==(Change const&) const = default;
};

// A controller whose AudioServer end stays open until the controller dies.
struct Controller {
    NonnullRefPtr<WebView::AudioServerControlClient> client;
    IPC::TransportHandle server_end;

    static Controller create()
    {
        auto transports = MUST(IPC::Transport::create_paired());
        return { adopt_ref(*new WebView::AudioServerControlClient(move(transports.local))), move(transports.remote_handle) };
    }

    void report(Audio::PlaybackGroupId group, Audio::Audibility audibility)
    {
        auto& stub = static_cast<AudioServerControlClientStub&>(*client);
        EXPECT(!stub.handle(make<Messages::AudioServerControlClient::PlaybackGroupAudibilityChanged>(TAB_ID, group, audibility)).is_error());
    }

    void report_capture(Audio::PlaybackGroupId group, bool capturing, u64 tab_id = TAB_ID)
    {
        auto& stub = static_cast<AudioServerControlClientStub&>(*client);
        EXPECT(!stub.handle(make<Messages::AudioServerControlClient::CaptureActivityChanged>(tab_id, group, capturing)).is_error());
    }

    void die() { client->shutdown(); }
};

struct ActivityFixture {
    Core::EventLoop loop;
    Vector<Change> changes;
    size_t capture_changes { 0 };
    WebView::AudioServerActivity activity;

    ActivityFixture()
        : activity([this](Audio::PlaybackGroupId group, Audio::Audibility audibility) { changes.append({ group, audibility }); }, [this] { ++capture_changes; })
    {
    }

    void watch(Controller& controller)
    {
        activity.watch(*controller.client, [](u64, Audio::PlaybackGroupId, Audio::Audibility) { return true; }, [](u64 tab_id) { return tab_id == TAB_ID; });
    }
};

TEST_CASE(a_replaced_audio_server_is_forgotten_at_once_and_heard_from_no_more)
{
    ActivityFixture fixture;
    auto replaced = Controller::create();
    fixture.watch(replaced);
    replaced.report(GROUP, AUDIBLE);
    EXPECT(fixture.changes == Vector<Change> { { GROUP, AUDIBLE } });

    // The replacement is watched before the replaced AudioServer's death is noticed.
    auto replacement = Controller::create();
    fixture.watch(replacement);
    EXPECT(fixture.changes == (Vector<Change> { { GROUP, AUDIBLE }, { GROUP, INAUDIBLE } }));

    replacement.report(OTHER_GROUP, AUDIBLE);
    replaced.report(GROUP, AUDIBLE);
    replaced.die();
    EXPECT(fixture.changes == (Vector<Change> { { GROUP, AUDIBLE }, { GROUP, INAUDIBLE }, { OTHER_GROUP, AUDIBLE } }));

    replacement.die();
    EXPECT(fixture.changes.last() == Change { OTHER_GROUP, INAUDIBLE });
}

TEST_CASE(a_dead_audio_servers_late_reports_are_ignored)
{
    ActivityFixture fixture;
    auto controller = Controller::create();
    fixture.watch(controller);
    controller.report(GROUP, AUDIBLE);
    controller.die();
    EXPECT(fixture.changes == (Vector<Change> { { GROUP, AUDIBLE }, { GROUP, INAUDIBLE } }));

    // A report the AudioServer sent before dying is dispatched after its death.
    controller.report(GROUP, AUDIBLE);
    EXPECT_EQ(fixture.changes.size(), 2u);
}

TEST_CASE(a_forgotten_group_is_inaudible_until_reported_audible_again)
{
    ActivityFixture fixture;
    auto controller = Controller::create();
    fixture.watch(controller);
    controller.report(GROUP, AUDIBLE);
    fixture.activity.forget_audibility(GROUP);
    fixture.activity.forget_audibility(GROUP);
    EXPECT(fixture.changes == (Vector<Change> { { GROUP, AUDIBLE }, { GROUP, INAUDIBLE } }));

    controller.report(GROUP, AUDIBLE);
    EXPECT(fixture.changes.last() == Change { GROUP, AUDIBLE });
}

TEST_CASE(reports_the_filter_turns_away_are_not_heard)
{
    ActivityFixture fixture;
    auto controller = Controller::create();
    fixture.activity.watch(*controller.client, [](u64, Audio::PlaybackGroupId group, Audio::Audibility) { return group == GROUP; }, [](u64) { return true; });
    controller.report(OTHER_GROUP, AUDIBLE);
    controller.report(GROUP, AUDIBLE);
    EXPECT(fixture.changes == Vector<Change> { { GROUP, AUDIBLE } });
}

TEST_CASE(capturing_groups_follow_the_reports_of_the_watched_tab)
{
    ActivityFixture fixture;
    auto controller = Controller::create();
    fixture.watch(controller);
    controller.report_capture(GROUP, true);
    controller.report_capture(GROUP, true);
    controller.report_capture(OTHER_GROUP, true, TAB_ID + 1);
    EXPECT(fixture.activity.capturing_groups().contains(GROUP));
    EXPECT_EQ(fixture.activity.capturing_groups().size(), 1u);
    EXPECT_EQ(fixture.capture_changes, 1u);

    controller.report_capture(GROUP, false);
    EXPECT(fixture.activity.capturing_groups().is_empty());
    EXPECT_EQ(fixture.capture_changes, 2u);
}

TEST_CASE(capture_goes_with_the_audio_server_but_not_with_a_forgotten_sound)
{
    ActivityFixture fixture;
    auto controller = Controller::create();
    fixture.watch(controller);
    controller.report_capture(GROUP, true);
    fixture.activity.forget_audibility(GROUP);
    EXPECT(fixture.activity.capturing_groups().contains(GROUP));

    controller.die();
    EXPECT(fixture.activity.capturing_groups().is_empty());
    EXPECT_EQ(fixture.capture_changes, 2u);
    controller.report_capture(GROUP, true);
    EXPECT(fixture.activity.capturing_groups().is_empty());
}
