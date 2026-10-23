/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibWebView/AudioServerAudibility.h>
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

    void die() { client->shutdown(); }
};

struct AudibilityFixture {
    Core::EventLoop loop;
    Vector<Change> changes;
    WebView::AudioServerAudibility audibility { [this](Audio::PlaybackGroupId group, Audio::Audibility audibility) {
        changes.append({ group, audibility });
    } };

    void watch(Controller& controller)
    {
        audibility.watch(*controller.client, [](u64, Audio::PlaybackGroupId, Audio::Audibility) { return true; });
    }
};

TEST_CASE(a_replaced_audio_server_is_forgotten_at_once_and_heard_from_no_more)
{
    AudibilityFixture fixture;
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
    AudibilityFixture fixture;
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
    AudibilityFixture fixture;
    auto controller = Controller::create();
    fixture.watch(controller);
    controller.report(GROUP, AUDIBLE);
    fixture.audibility.forget(GROUP);
    fixture.audibility.forget(GROUP);
    EXPECT(fixture.changes == (Vector<Change> { { GROUP, AUDIBLE }, { GROUP, INAUDIBLE } }));

    controller.report(GROUP, AUDIBLE);
    EXPECT(fixture.changes.last() == Change { GROUP, AUDIBLE });
}

TEST_CASE(reports_the_filter_turns_away_are_not_heard)
{
    AudibilityFixture fixture;
    auto controller = Controller::create();
    fixture.audibility.watch(*controller.client, [](u64, Audio::PlaybackGroupId group, Audio::Audibility) { return group == GROUP; });
    controller.report(OTHER_GROUP, AUDIBLE);
    controller.report(GROUP, AUDIBLE);
    EXPECT(fixture.changes == Vector<Change> { { GROUP, AUDIBLE } });
}
