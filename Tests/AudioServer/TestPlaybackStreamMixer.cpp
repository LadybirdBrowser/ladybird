/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FakeDeviceStream.h"
#include <AK/Vector.h>
#include <AudioServer/PlaybackStreamMixer.h>
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>

static constexpr u32 SAMPLE_RATE = FakeDeviceStream::SAMPLE_RATE;
static constexpr u32 CHANNEL_COUNT = FakeDeviceStream::CHANNEL_COUNT;
static constexpr Audio::PlaybackGroupId AUTHORIZED_GROUP { 1 };
static constexpr Audio::PlaybackGroupId OTHER_GROUP { 2 };

struct AudibilityReport {
    Audio::PlaybackGroupId group;
    Audio::Audibility audibility;
    bool operator==(AudibilityReport const&) const = default;
};

struct MixerFixture {
    Core::EventLoop loop;
    RefPtr<FakeDeviceStream> device;
    RefPtr<Audio::PlaybackStreamMixer> mixer;
    Vector<AudibilityReport> audibility_reports;

    explicit MixerFixture(AK::Duration audibility_linger = Audio::AUDIBILITY_LINGER)
    {
        mixer = Audio::PlaybackStreamMixer::create(
            loop, 100, [this](Audio::OutputState state, u32, Audio::PlaybackStream::AudioDataRequestCallback callback) {
                device = make_ref_counted<FakeDeviceStream>(state, move(callback));
                auto promise = Audio::PlaybackStream::CreatePromise::construct();
                promise->resolve(*device);
                return promise;
            },
            audibility_linger);
        EXPECT(device);
        EXPECT(mixer->device_sample_specification().has_value());
        mixer->set_group_authorization(AUTHORIZED_GROUP, Audio::PlaybackGroupAuthorization::Authorized);
        mixer->on_group_audibility_changed = [this](Audio::PlaybackGroupId group, Audio::Audibility audibility) {
            audibility_reports.append({ group, audibility });
        };
    }

    // Renders the frames, then hands the control thread whatever the render posted to it.
    void render_and_report(size_t frame_count)
    {
        (void)device->render(frame_count);
        pump();
    }

    // The client side of a ring, registered with the mixer under `id`.
    Audio::SharedAudioFrameRing add_client(Audio::MixerClientId id, float gain = 1.0f, Optional<Audio::PlaybackGroupId> group = AUTHORIZED_GROUP)
    {
        auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 1024));
        auto consumer = MUST(Audio::SharedAudioFrameRing::create(producer.buffer()));
        mixer->add_client(id, move(consumer), gain, group);
        return producer;
    }

    void expect_render_of(size_t frame_count, float expected)
    {
        for (auto sample : device->render(frame_count))
            EXPECT_EQ(sample, expected);
    }

    void pump()
    {
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    template<typename Condition>
    bool pump_until(Condition condition, AK::Duration timeout = AK::Duration::from_seconds(5))
    {
        auto deadline = MonotonicTime::now() + timeout;
        while (!condition()) {
            if (MonotonicTime::now() > deadline)
                return false;
            pump();
        }
        return true;
    }
};

static Vector<float> constant_frames(size_t frame_count, float value)
{
    Vector<float> samples;
    samples.resize(frame_count * CHANNEL_COUNT);
    samples.fill(value);
    return samples;
}

TEST_CASE(device_opens_suspended_and_reports_its_format)
{
    MixerFixture fixture;
    EXPECT(!fixture.device->is_playing());
    EXPECT_EQ(fixture.mixer->device_sample_specification()->sample_rate(), SAMPLE_RATE);
    EXPECT_EQ(fixture.mixer->device_sample_specification()->channel_count(), CHANNEL_COUNT);
    EXPECT(!fixture.mixer->has_clients());
}

TEST_CASE(playing_clients_are_summed_with_their_gain)
{
    MixerFixture fixture;
    auto first = fixture.add_client(1, 1.0f);
    auto second = fixture.add_client(2, 0.5f);
    EXPECT_EQ(first.try_push(constant_frames(64, 0.25f)), 64u);
    EXPECT_EQ(second.try_push(constant_frames(64, 1.0f)), 64u);

    fixture.mixer->resume_client(1);
    fixture.mixer->resume_client(2);
    auto output = fixture.device->render(64);
    for (auto sample : output)
        EXPECT_EQ(sample, 0.25f + 0.5f);
    EXPECT_EQ(first.frames_free(), 1024u);
    EXPECT_EQ(second.frames_free(), 1024u);
}

TEST_CASE(clients_outside_an_authorized_group_are_consumed_into_silence)
{
    MixerFixture fixture;
    auto authorized = fixture.add_client(1, 1.0f, AUTHORIZED_GROUP);
    auto unauthorized = fixture.add_client(2, 1.0f, OTHER_GROUP);
    auto ungrouped = fixture.add_client(3, 1.0f, OptionalNone {});
    EXPECT_EQ(authorized.try_push(constant_frames(32, 0.25f)), 32u);
    EXPECT_EQ(unauthorized.try_push(constant_frames(32, 1.0f)), 32u);
    EXPECT_EQ(ungrouped.try_push(constant_frames(32, 1.0f)), 32u);

    fixture.mixer->resume_client(1);
    fixture.mixer->resume_client(2);
    fixture.mixer->resume_client(3);
    fixture.expect_render_of(32, 0.25f);
    // They play on unheard, at the device's pace.
    EXPECT_EQ(unauthorized.frames_free(), 1024u);
    EXPECT_EQ(ungrouped.frames_free(), 1024u);
}

TEST_CASE(a_group_is_heard_from_the_render_after_its_authorization_until_its_revocation)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1, 1.0f, OTHER_GROUP);
    EXPECT_EQ(client.try_push(constant_frames(96, 1.0f)), 96u);
    fixture.mixer->resume_client(1);

    fixture.expect_render_of(32, 0.0f);
    fixture.mixer->set_group_authorization(OTHER_GROUP, Audio::PlaybackGroupAuthorization::Authorized);
    fixture.expect_render_of(32, 1.0f);
    fixture.mixer->set_group_authorization(OTHER_GROUP, Audio::PlaybackGroupAuthorization::Unauthorized);
    fixture.expect_render_of(32, 0.0f);
}

TEST_CASE(a_client_moved_to_another_group_is_heard_as_part_of_it)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1, 1.0f, OTHER_GROUP);
    EXPECT_EQ(client.try_push(constant_frames(96, 1.0f)), 96u);
    fixture.mixer->resume_client(1);

    fixture.expect_render_of(32, 0.0f);
    fixture.mixer->set_client_group(1, AUTHORIZED_GROUP);
    fixture.expect_render_of(32, 1.0f);
    fixture.mixer->set_client_group(1, OptionalNone {});
    fixture.expect_render_of(32, 0.0f);
}

TEST_CASE(a_muted_group_is_consumed_into_silence_until_it_is_unmuted)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(64, 1.0f)), 64u);
    fixture.mixer->resume_client(1);

    fixture.mixer->set_group_mute_state(AUTHORIZED_GROUP, Audio::PlaybackGroupMuteState::Muted);
    fixture.expect_render_of(32, 0.0f);
    EXPECT_EQ(client.frames_free(), 1024u - 32u);
    fixture.mixer->set_group_mute_state(AUTHORIZED_GROUP, Audio::PlaybackGroupMuteState::Unmuted);
    fixture.expect_render_of(32, 1.0f);
}

TEST_CASE(muting_and_authorizing_a_group_are_independent)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1, 1.0f, OTHER_GROUP);
    EXPECT_EQ(client.try_push(constant_frames(128, 1.0f)), 128u);
    fixture.mixer->resume_client(1);

    // Authorizing a muted group leaves it muted.
    fixture.mixer->set_group_mute_state(OTHER_GROUP, Audio::PlaybackGroupMuteState::Muted);
    fixture.mixer->set_group_authorization(OTHER_GROUP, Audio::PlaybackGroupAuthorization::Authorized);
    fixture.expect_render_of(32, 0.0f);
    fixture.mixer->set_group_mute_state(OTHER_GROUP, Audio::PlaybackGroupMuteState::Unmuted);
    fixture.expect_render_of(32, 1.0f);

    // Unmuting a revoked group leaves it unauthorized.
    fixture.mixer->set_group_authorization(OTHER_GROUP, Audio::PlaybackGroupAuthorization::Unauthorized);
    fixture.mixer->set_group_mute_state(OTHER_GROUP, Audio::PlaybackGroupMuteState::Muted);
    fixture.mixer->set_group_mute_state(OTHER_GROUP, Audio::PlaybackGroupMuteState::Unmuted);
    fixture.expect_render_of(32, 0.0f);
}

TEST_CASE(suspended_clients_are_not_consumed)
{
    MixerFixture fixture;
    auto playing = fixture.add_client(1);
    auto suspended = fixture.add_client(2);
    EXPECT_EQ(playing.try_push(constant_frames(32, 1.0f)), 32u);
    EXPECT_EQ(suspended.try_push(constant_frames(32, 1.0f)), 32u);

    fixture.mixer->resume_client(1);
    auto output = fixture.device->render(32);
    for (auto sample : output)
        EXPECT_EQ(sample, 1.0f);
    EXPECT_EQ(playing.frames_free(), 1024u);
    EXPECT_EQ(suspended.frames_free(), 1024u - 32u);
}

TEST_CASE(starved_client_leaves_silence_and_a_non_playing_anchor)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(16, 1.0f)), 16u);
    fixture.mixer->resume_client(1);

    auto output = fixture.device->render(32);
    for (size_t i = 0; i < 16 * CHANNEL_COUNT; ++i)
        EXPECT_EQ(output[i], 1.0f);
    for (size_t i = 16 * CHANNEL_COUNT; i < output.size(); ++i)
        EXPECT_EQ(output[i], 0.0f);

    // The callback consumed 16 frames, which the device (reporting no latency) has played a frame's time later; the
    // client's time stands at 16 then and may extrapolate from there.
    auto anchor_time = MonotonicTime::now().nanoseconds() + 1'000'000;
    EXPECT(client.estimate_frames_played(anchor_time) >= 16u);

    // Nothing left to consume: the next callback publishes a non-playing anchor at 16 frames.
    (void)fixture.device->render(32);
    EXPECT_EQ(client.estimate_frames_played(anchor_time + 1'000'000'000), 16u);
}

TEST_CASE(anchors_are_dated_from_when_the_device_plays_the_buffer)
{
    MixerFixture fixture;
    fixture.device->latency = AK::Duration::from_milliseconds(100);
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(48, 1.0f)), 48u);
    fixture.mixer->resume_client(1);

    auto before_render = MonotonicTime::now().nanoseconds();
    (void)fixture.device->render(32);

    // The device holds 100 ms ahead of this buffer, so none of it has been heard yet, nor will it be in 50 ms.
    EXPECT_EQ(client.estimate_frames_played(before_render), 0u);
    EXPECT_EQ(client.estimate_frames_played(before_render + 50'000'000), 0u);
    // Once the latency and the buffer have passed, the 32 frames have played; extrapolation may go on to the 48.
    auto played = client.estimate_frames_played(before_render + 101'000'000 + AK::Duration::from_time_units(32, 1, SAMPLE_RATE).to_nanoseconds());
    EXPECT(played >= 32u);
    EXPECT(played <= 48u);
}

TEST_CASE(drain_completes_only_once_the_device_has_played_the_tail)
{
    MixerFixture fixture;
    fixture.device->latency = AK::Duration::from_milliseconds(60);
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(32, 1.0f)), 32u);
    fixture.mixer->resume_client(1);

    bool drained = false;
    fixture.mixer->drain_client(1, [&] { drained = true; });
    auto emptied_at = MonotonicTime::now();
    (void)fixture.device->render(32);
    fixture.pump();
    // The ring is empty, but the device still has to play those frames out.
    EXPECT(!drained);
    EXPECT(fixture.mixer->active_client_count() == 1u);
    EXPECT(fixture.pump_until([&] { return drained; }));
    EXPECT(MonotonicTime::now() - emptied_at >= AK::Duration::from_milliseconds(55));
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);
}

TEST_CASE(device_plays_only_while_a_client_is_active)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(fixture.device->resume_count, 0u);

    fixture.mixer->resume_client(1);
    EXPECT(fixture.device->is_playing());
    EXPECT_EQ(fixture.device->resume_count, 1u);
    EXPECT_EQ(fixture.mixer->active_client_count(), 1u);

    // A second active client does not resume the device again.
    auto other = fixture.add_client(2);
    fixture.mixer->resume_client(2);
    EXPECT_EQ(fixture.device->resume_count, 1u);
    EXPECT_EQ(fixture.mixer->active_client_count(), 2u);

    fixture.mixer->discard_client(2);
    EXPECT(fixture.device->is_playing());
    fixture.mixer->discard_client(1);
    EXPECT(!fixture.device->is_playing());
    EXPECT_EQ(fixture.device->drain_count, 1u);
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);

    // Resuming again reopens.
    fixture.mixer->resume_client(1);
    EXPECT(fixture.device->is_playing());
    EXPECT_EQ(fixture.device->resume_count, 2u);
}

TEST_CASE(drain_completes_once_the_ring_has_played_out)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(48, 1.0f)), 48u);
    fixture.mixer->resume_client(1);

    bool drained = false;
    fixture.mixer->drain_client(1, [&] { drained = true; });
    fixture.pump();
    EXPECT(!drained);
    EXPECT(fixture.device->is_playing());

    // The first callback takes 32 of the 48 frames: still draining.
    auto output = fixture.device->render(32);
    for (auto sample : output)
        EXPECT_EQ(sample, 1.0f);
    fixture.pump();
    EXPECT(!drained);

    // The second takes the remaining 16 and completes the drain once they have played; the device then goes idle.
    output = fixture.device->render(32);
    for (size_t i = 0; i < 16 * CHANNEL_COUNT; ++i)
        EXPECT_EQ(output[i], 1.0f);
    EXPECT(fixture.pump_until([&] { return drained; }));
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);
    EXPECT(!fixture.device->is_playing());
}

TEST_CASE(draining_a_suspended_client_completes_at_once)
{
    MixerFixture fixture;
    (void)fixture.add_client(1);
    bool drained = false;
    fixture.mixer->drain_client(1, [&] { drained = true; });
    EXPECT(drained);
    EXPECT(!fixture.device->is_playing());
}

TEST_CASE(resuming_during_a_drain_cancels_it_and_completes_the_request)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(16, 1.0f)), 16u);
    fixture.mixer->resume_client(1);

    size_t completions = 0;
    fixture.mixer->drain_client(1, [&] { completions++; });
    fixture.mixer->resume_client(1);
    EXPECT_EQ(completions, 1u);

    // The client keeps playing; the callback now consumes its frames without finishing a drain.
    (void)fixture.device->render(32);
    fixture.pump();
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(fixture.mixer->active_client_count(), 1u);
    EXPECT(fixture.device->is_playing());
}

TEST_CASE(discarding_during_a_drain_completes_the_request)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(16, 1.0f)), 16u);
    fixture.mixer->resume_client(1);

    size_t completions = 0;
    fixture.mixer->drain_client(1, [&] { completions++; });
    fixture.mixer->discard_client(1);
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);
    EXPECT(!fixture.device->is_playing());
}

TEST_CASE(discard_drops_buffered_frames)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(100, 1.0f)), 100u);
    fixture.mixer->resume_client(1);
    fixture.mixer->discard_client(1);
    EXPECT_EQ(client.frames_free(), 1024u);
    EXPECT_EQ(client.frames_written(), 100u);

    auto output = fixture.device->render(32);
    for (auto sample : output)
        EXPECT_EQ(sample, 0.0f);
}

TEST_CASE(removing_an_active_client_releases_the_device)
{
    MixerFixture fixture;
    (void)fixture.add_client(1);
    fixture.mixer->resume_client(1);
    EXPECT(fixture.device->is_playing());
    fixture.mixer->remove_client(1);
    EXPECT(!fixture.device->is_playing());
    EXPECT(!fixture.mixer->has_clients());
}

TEST_CASE(gain_changes_apply_to_the_next_callback)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1, 1.0f);
    EXPECT_EQ(client.try_push(constant_frames(64, 1.0f)), 64u);
    fixture.mixer->resume_client(1);
    fixture.mixer->set_client_gain(1, 0.25f);
    auto output = fixture.device->render(64);
    for (auto sample : output)
        EXPECT_EQ(sample, 0.25f);
}

TEST_CASE(the_mix_is_clipped_to_what_devices_play)
{
    MixerFixture fixture;
    auto first = fixture.add_client(1);
    auto second = fixture.add_client(2);
    fixture.mixer->resume_client(1);
    fixture.mixer->resume_client(2);

    auto mix = [&](float first_sample, float second_sample) {
        EXPECT_EQ(first.try_push(constant_frames(64, first_sample)), 64u);
        EXPECT_EQ(second.try_push(constant_frames(64, second_sample)), 64u);
        return fixture.device->render(64);
    };
    for (auto sample : mix(0.75f, 0.75f))
        EXPECT_EQ(sample, 1.0f);
    for (auto sample : mix(-0.75f, -0.75f))
        EXPECT_EQ(sample, -1.0f);
    for (auto sample : mix(0.25f, 0.5f))
        EXPECT_EQ(sample, 0.75f);

    // Sound beyond the range can still be cancelled out by the rest of the mix.
    for (auto sample : mix(1.5f, -1.0f))
        EXPECT_EQ(sample, 0.5f);
}

TEST_CASE(resuming_a_client_whose_ring_ran_empty_does_not_count_it_twice)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(16, 1.0f)), 16u);
    fixture.mixer->resume_client(1);

    bool drained = false;
    fixture.mixer->drain_client(1, [&] { drained = true; });
    // The callback empties the ring, but its completion has not reached the control thread yet.
    (void)fixture.device->render(32);
    fixture.mixer->resume_client(1);
    EXPECT(drained);
    fixture.pump();
    EXPECT_EQ(fixture.mixer->active_client_count(), 1u);

    fixture.mixer->discard_client(1);
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);
    EXPECT(!fixture.device->is_playing());
}

TEST_CASE(discarding_a_client_whose_ring_ran_empty_stops_the_device)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(16, 1.0f)), 16u);
    fixture.mixer->resume_client(1);

    bool drained = false;
    fixture.mixer->drain_client(1, [&] { drained = true; });
    (void)fixture.device->render(32);
    fixture.mixer->discard_client(1);
    EXPECT(drained);
    fixture.pump();
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);
    EXPECT(!fixture.device->is_playing());

    // The same for a client that leaves altogether.
    auto other = fixture.add_client(2);
    EXPECT_EQ(other.try_push(constant_frames(16, 1.0f)), 16u);
    fixture.mixer->resume_client(2);
    fixture.mixer->drain_client(2, [] { });
    (void)fixture.device->render(32);
    fixture.mixer->remove_client(2);
    fixture.pump();
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);
    EXPECT(!fixture.device->is_playing());
}

TEST_CASE(a_second_drain_request_completes_along_with_the_first)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(16, 1.0f)), 16u);
    fixture.mixer->resume_client(1);

    size_t completions = 0;
    fixture.mixer->drain_client(1, [&] { completions++; });
    fixture.mixer->drain_client(1, [&] { completions++; });
    (void)fixture.device->render(32);
    EXPECT(fixture.pump_until([&] { return completions == 2; }));
    EXPECT_EQ(fixture.mixer->active_client_count(), 0u);
    EXPECT(!fixture.device->is_playing());
}

static constexpr float THRESHOLD = Audio::AUDIBLE_PEAK;
static constexpr auto AUDIBLE = Audio::Audibility::Audible;
static constexpr auto INAUDIBLE = Audio::Audibility::Inaudible;
static constexpr Audio::PlaybackGroupId THIRD_GROUP { 3 };

TEST_CASE(a_group_is_audible_from_the_first_buffer_that_reaches_the_threshold)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(32, THRESHOLD * 0.99f)), 32u);
    EXPECT_EQ(client.try_push(constant_frames(32, -THRESHOLD)), 32u);
    fixture.mixer->resume_client(1);

    fixture.render_and_report(32);
    EXPECT(fixture.audibility_reports.is_empty());
    fixture.render_and_report(32);
    EXPECT(fixture.audibility_reports == Vector<AudibilityReport> { { AUTHORIZED_GROUP, AUDIBLE } });
}

TEST_CASE(a_groups_streams_are_measured_together)
{
    MixerFixture fixture;
    auto first = fixture.add_client(1);
    auto second = fixture.add_client(2);
    EXPECT_EQ(first.try_push(constant_frames(32, THRESHOLD * 2)), 32u);
    EXPECT_EQ(second.try_push(constant_frames(32, -THRESHOLD * 2)), 32u);
    EXPECT_EQ(first.try_push(constant_frames(32, THRESHOLD * 0.6f)), 32u);
    EXPECT_EQ(second.try_push(constant_frames(32, THRESHOLD * 0.6f)), 32u);
    fixture.mixer->resume_client(1);
    fixture.mixer->resume_client(2);

    // Streams that cancel each other out are silent together, and streams too quiet on their own are not.
    fixture.render_and_report(32);
    EXPECT(fixture.audibility_reports.is_empty());
    fixture.render_and_report(32);
    EXPECT(fixture.audibility_reports == Vector<AudibilityReport> { { AUTHORIZED_GROUP, AUDIBLE } });
}

TEST_CASE(a_group_is_measured_after_its_streams_gain_and_muted_or_not)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1, 0.5f);
    EXPECT_EQ(client.try_push(constant_frames(32, THRESHOLD * 1.5f)), 32u);
    EXPECT_EQ(client.try_push(constant_frames(32, THRESHOLD * 4)), 32u);
    fixture.mixer->set_group_mute_state(AUTHORIZED_GROUP, Audio::PlaybackGroupMuteState::Muted);
    fixture.mixer->resume_client(1);

    fixture.render_and_report(32);
    EXPECT(fixture.audibility_reports.is_empty());
    fixture.expect_render_of(32, 0.0f);
    fixture.pump();
    EXPECT(fixture.audibility_reports == Vector<AudibilityReport> { { AUTHORIZED_GROUP, AUDIBLE } });
}

TEST_CASE(groups_too_quiet_on_their_own_are_audible_while_their_total_is)
{
    MixerFixture fixture;
    fixture.mixer->set_group_authorization(OTHER_GROUP, Audio::PlaybackGroupAuthorization::Authorized);
    fixture.mixer->set_group_authorization(THIRD_GROUP, Audio::PlaybackGroupAuthorization::Authorized);
    auto loud = fixture.add_client(1, 1.0f, AUTHORIZED_GROUP);
    auto quiet = fixture.add_client(2, 1.0f, OTHER_GROUP);
    auto other_quiet = fixture.add_client(3, 1.0f, THIRD_GROUP);
    EXPECT_EQ(loud.try_push(constant_frames(32, THRESHOLD * 2)), 32u);
    EXPECT_EQ(quiet.try_push(constant_frames(64, THRESHOLD * 0.6f)), 64u);
    EXPECT_EQ(other_quiet.try_push(constant_frames(64, THRESHOLD * 0.6f)), 64u);
    fixture.mixer->resume_client(1);
    fixture.mixer->resume_client(2);
    fixture.mixer->resume_client(3);

    // While a group is audible on its own, the quiet ones are not counted.
    fixture.render_and_report(32);
    EXPECT(fixture.audibility_reports == Vector<AudibilityReport> { { AUTHORIZED_GROUP, AUDIBLE } });
    fixture.render_and_report(32);
    EXPECT_EQ(fixture.audibility_reports.size(), 3u);
    EXPECT(fixture.audibility_reports.contains_slow(AudibilityReport { OTHER_GROUP, AUDIBLE }));
    EXPECT(fixture.audibility_reports.contains_slow(AudibilityReport { THIRD_GROUP, AUDIBLE }));
}

// Two unmuted groups too quiet on their own, and a muted group, which plays nothing.
static void play_two_quiet_groups_alongside_a_muted_one(MixerFixture& fixture, float muted_sample)
{
    fixture.mixer->set_group_authorization(OTHER_GROUP, Audio::PlaybackGroupAuthorization::Authorized);
    fixture.mixer->set_group_authorization(THIRD_GROUP, Audio::PlaybackGroupAuthorization::Authorized);
    fixture.mixer->set_group_mute_state(THIRD_GROUP, Audio::PlaybackGroupMuteState::Muted);
    auto quiet = fixture.add_client(1, 1.0f, AUTHORIZED_GROUP);
    auto other_quiet = fixture.add_client(2, 1.0f, OTHER_GROUP);
    auto muted = fixture.add_client(3, 1.0f, THIRD_GROUP);
    EXPECT_EQ(quiet.try_push(constant_frames(32, THRESHOLD * 0.75f)), 32u);
    EXPECT_EQ(other_quiet.try_push(constant_frames(32, THRESHOLD * 0.75f)), 32u);
    EXPECT_EQ(muted.try_push(constant_frames(32, muted_sample)), 32u);
    fixture.mixer->resume_client(1);
    fixture.mixer->resume_client(2);
    fixture.mixer->resume_client(3);
    fixture.render_and_report(32);
}

TEST_CASE(an_audible_muted_group_does_not_hide_quiet_groups_heard_together)
{
    MixerFixture fixture;
    play_two_quiet_groups_alongside_a_muted_one(fixture, THRESHOLD * 2);
    EXPECT_EQ(fixture.audibility_reports.size(), 3u);
    EXPECT(fixture.audibility_reports.contains_slow(AudibilityReport { AUTHORIZED_GROUP, AUDIBLE }));
    EXPECT(fixture.audibility_reports.contains_slow(AudibilityReport { OTHER_GROUP, AUDIBLE }));
    EXPECT(fixture.audibility_reports.contains_slow(AudibilityReport { THIRD_GROUP, AUDIBLE }));
}

TEST_CASE(a_muted_group_does_not_cancel_what_quiet_groups_play_together)
{
    MixerFixture fixture;
    play_two_quiet_groups_alongside_a_muted_one(fixture, -THRESHOLD * 0.75f);
    EXPECT_EQ(fixture.audibility_reports.size(), 2u);
    EXPECT(fixture.audibility_reports.contains_slow(AudibilityReport { AUTHORIZED_GROUP, AUDIBLE }));
    EXPECT(fixture.audibility_reports.contains_slow(AudibilityReport { OTHER_GROUP, AUDIBLE }));
}

TEST_CASE(a_group_stays_audible_until_the_linger_has_passed_since_it_was_last_heard)
{
    static constexpr auto LINGER = AK::Duration::from_milliseconds(200);
    MixerFixture fixture(LINGER);
    auto client = fixture.add_client(1);
    fixture.mixer->resume_client(1);

    auto heard_since = MonotonicTime::now();
    auto last_heard_at = heard_since;
    while (MonotonicTime::now() - heard_since < LINGER + LINGER) {
        (void)client.try_push(constant_frames(32, 1.0f));
        last_heard_at = MonotonicTime::now();
        fixture.render_and_report(32);
    }
    EXPECT(fixture.audibility_reports == Vector<AudibilityReport> { { AUTHORIZED_GROUP, AUDIBLE } });

    // Nothing more is rendered, as when the device has stopped.
    EXPECT(fixture.pump_until([&] { return fixture.audibility_reports.size() == 2; }));
    EXPECT(MonotonicTime::now() - last_heard_at >= LINGER);
    EXPECT(fixture.audibility_reports.last() == AudibilityReport { AUTHORIZED_GROUP, INAUDIBLE });
}

TEST_CASE(revoking_an_audible_group_makes_it_inaudible_at_once)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(32, 1.0f)), 32u);
    fixture.mixer->resume_client(1);
    fixture.render_and_report(32);

    fixture.mixer->set_group_authorization(AUTHORIZED_GROUP, Audio::PlaybackGroupAuthorization::Unauthorized);
    EXPECT(fixture.audibility_reports == (Vector<AudibilityReport> { { AUTHORIZED_GROUP, AUDIBLE }, { AUTHORIZED_GROUP, INAUDIBLE } }));
}

TEST_CASE(a_group_revoked_before_its_sound_is_reported_is_never_reported_audible)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(32, 1.0f)), 32u);
    fixture.mixer->resume_client(1);
    (void)fixture.device->render(32);

    fixture.mixer->set_group_authorization(AUTHORIZED_GROUP, Audio::PlaybackGroupAuthorization::Unauthorized);
    fixture.pump();
    EXPECT(fixture.audibility_reports.is_empty());
}

TEST_CASE(only_authorized_groups_are_measured)
{
    MixerFixture fixture;
    auto unauthorized = fixture.add_client(1, 1.0f, OTHER_GROUP);
    auto ungrouped = fixture.add_client(2, 1.0f, OptionalNone {});
    EXPECT_EQ(unauthorized.try_push(constant_frames(32, 1.0f)), 32u);
    EXPECT_EQ(ungrouped.try_push(constant_frames(32, 1.0f)), 32u);
    fixture.mixer->resume_client(1);
    fixture.mixer->resume_client(2);

    fixture.render_and_report(32);
    EXPECT(fixture.audibility_reports.is_empty());
}

TEST_CASE(letting_go_of_the_mixer_reports_every_audible_group_inaudible)
{
    MixerFixture fixture;
    auto client = fixture.add_client(1);
    EXPECT_EQ(client.try_push(constant_frames(32, 1.0f)), 32u);
    fixture.mixer->resume_client(1);
    fixture.render_and_report(32);

    fixture.mixer->report_every_group_inaudible();
    EXPECT(fixture.audibility_reports == (Vector<AudibilityReport> { { AUTHORIZED_GROUP, AUDIBLE }, { AUTHORIZED_GROUP, INAUDIBLE } }));
}
