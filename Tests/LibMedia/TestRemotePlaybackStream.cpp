/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FakeDeviceStream.h"
#include <AK/Atomic.h>
#include <AK/Mutex.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibMedia/Audio/ClientConnection.h>
#include <LibMedia/Audio/PlaybackStreamMixer.h>
#include <LibMedia/Audio/RemotePlaybackStream.h>
#include <LibMedia/Audio/ServerConnection.h>
#include <LibTest/TestCase.h>

static constexpr u32 CHANNEL_COUNT = FakeDeviceStream::CHANNEL_COUNT;

// What the data request callback hands out. Declared before the fixture, since the pump may still be in the callback
// when a test drops its stream.
struct Source {
    struct Request {
        MonotonicTime requested_at;
        MonotonicTime buffer_starts_playing_at;
        u64 frames_supplied_before { 0 };
    };

    Atomic<bool> starved { false };
    Atomic<u64> requests { 0 };
    Atomic<u64> frames_supplied { 0 };
    Mutex request_mutex;
    Optional<Request> latest_request;
    float value { 1.0f };
    // Runs on the pump thread inside a starved callback, before it returns empty.
    Function<void()> on_starved_request;

    Request latest()
    {
        MutexLocker locker(request_mutex);
        return latest_request.value();
    }

    Audio::PlaybackStream::AudioDataRequestCallback callback()
    {
        return [this](Span<float> buffer, MonotonicTime buffer_starts_playing_at) -> ReadonlySpan<float> {
            {
                MutexLocker locker(request_mutex);
                latest_request = Request { MonotonicTime::now(), buffer_starts_playing_at, frames_supplied.load() };
            }
            requests++;
            if (starved.load()) {
                if (on_starved_request)
                    on_starved_request();
                return {};
            }
            buffer.fill(value);
            frames_supplied += buffer.size() / CHANNEL_COUNT;
            return buffer;
        };
    }
};

// A mixer on a fake device, a server connection serving it, and a client connection on the other end of a paired
// transport, all on one event loop.
struct RemoteFixture {
    static constexpr u32 TARGET_LATENCY_MS = 100;
    static constexpr u32 DEVICE_LATENCY_MS = 40;
    // What the client keeps in its ring: the budget less the device's share.
    static constexpr u32 RING_LATENCY_MS = TARGET_LATENCY_MS - DEVICE_LATENCY_MS;

    Core::EventLoop loop;
    RefPtr<FakeDeviceStream> device;
    RefPtr<Audio::PlaybackStreamMixer> mixer;
    RefPtr<Audio::ServerConnection> server;
    WeakPtr<Audio::ServerConnection> server_weak;
    RefPtr<Audio::ClientConnection> client;
    WeakPtr<Audio::ClientConnection> client_weak;
    bool server_died { false };

    RemoteFixture()
    {
        mixer = Audio::PlaybackStreamMixer::create(loop, DEVICE_LATENCY_MS, [this](Audio::OutputState state, u32, Audio::PlaybackStream::AudioDataRequestCallback callback) {
            device = make_ref_counted<FakeDeviceStream>(state, move(callback));
            auto promise = Audio::PlaybackStream::CreatePromise::construct();
            promise->resolve(*device);
            return promise;
        });
        auto paired = MUST(IPC::Transport::create_paired());
        server = Audio::ServerConnection::construct(move(paired.local), 1, *mixer);
        server->on_death = [this] { server_died = true; };
        server_weak = server->make_weak_ptr<Audio::ServerConnection>();
        client = adopt_ref(*new Audio::ClientConnection(MUST(paired.remote_handle.create_transport())));
        client_weak = client->make_weak_ptr<Audio::ClientConnection>();
        introduce_paired_transports(server->transport(), client->transport());
    }

    // The client goes first, and the loop runs until both ends are gone for good: a stream's last reference may be the
    // pump's, whose goodbye is deferred on this loop, and the server's shutdown is deferred with a reference of its own.
    ~RemoteFixture()
    {
        client = nullptr;
        EXPECT(pump_until([&] { return client_weak.is_null(); }));
        if (server)
            EXPECT(pump_until([&] { return server_died; }));
        server = nullptr;
        EXPECT(pump_until([&] { return server_weak.is_null(); }));
    }

    template<typename Condition>
    bool pump_until(Condition condition, AK::Duration timeout = AK::Duration::from_seconds(5))
    {
        auto deadline = MonotonicTime::now() + timeout;
        while (!condition()) {
            if (MonotonicTime::now() > deadline)
                return false;
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return true;
    }

    RefPtr<Audio::PlaybackStream> create_stream(Audio::OutputState initial_state, Source& source)
    {
        RefPtr<Audio::PlaybackStream> stream;
        bool rejected = false;
        auto promise = client->create_stream(initial_state, TARGET_LATENCY_MS, source.callback());
        promise->when_resolved([&](NonnullRefPtr<Audio::PlaybackStream>& created) { stream = created; });
        promise->when_rejected([&](Error&) { rejected = true; });
        EXPECT(pump_until([&] { return stream || rejected; }));
        EXPECT(!rejected);
        return stream;
    }

    // Keeps the fake device rendering while waiting, the way a real device would.
    template<typename Condition>
    bool render_until(Condition condition, size_t frames_per_render = 480)
    {
        return pump_until([&] {
            if (device->is_playing())
                (void)device->render(frames_per_render);
            return condition();
        });
    }

    // Renders until the pump has filled a whole buffer with the expected value; the pump pushes into the ring some
    // time after the data request callback returns.
    bool render_until_full_buffer_of(float expected, size_t frames_per_render = 480)
    {
        return pump_until([&] {
            if (!device->is_playing())
                return false;
            auto output = device->render(frames_per_render);
            return all_of(output, [&](float sample) { return sample == expected; });
        });
    }
};

TEST_CASE(stream_is_created_with_the_device_format)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    EXPECT(stream);
    EXPECT_EQ(stream->sample_specification().sample_rate(), FakeDeviceStream::SAMPLE_RATE);
    EXPECT_EQ(stream->sample_specification().channel_count(), CHANNEL_COUNT);
    // Nothing is attached to the mixer until the stream plays.
    EXPECT(!fixture.mixer->has_clients());
    EXPECT_EQ(source.requests.load(), 0u);
}

TEST_CASE(resume_attaches_a_ring_that_the_pump_fills)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);

    bool resumed = false;
    stream->resume()->when_resolved([&] { resumed = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.pump_until([&] { return resumed && fixture.mixer->has_clients() && fixture.device->is_playing(); }));
    EXPECT(fixture.render_until_full_buffer_of(1.0f));
}

TEST_CASE(pump_dates_each_buffer_by_what_the_ring_still_holds)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();

    // The first buffer fills the empty ring up to its share of the budget, so it plays at once.
    auto target_frames = AK::Duration::from_milliseconds(RemoteFixture::RING_LATENCY_MS).to_time_units(1, FakeDeviceStream::SAMPLE_RATE);
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= static_cast<u64>(target_frames); }));
    auto first_request = source.latest();
    EXPECT(first_request.buffer_starts_playing_at <= first_request.requested_at);
    auto requests_before_consuming = source.requests.load();

    // Renders until the device takes some of that fill: a push lands whole, so until then it takes nothing.
    auto rendered_at = MonotonicTime::now();
    u64 frames_consumed = 0;
    u64 frames_consumed_before_render = 0;
    EXPECT(fixture.pump_until([&] {
        if (!fixture.device->is_playing())
            return false;
        frames_consumed_before_render = frames_consumed;
        rendered_at = MonotonicTime::now();
        u64 consumed_samples = 0;
        for (auto sample : fixture.device->render(480)) {
            if (sample == 1.0f)
                consumed_samples++;
        }
        frames_consumed += consumed_samples / CHANNEL_COUNT;
        return frames_consumed > frames_consumed_before_render;
    }));

    // However late the pump comes around, it dates the replacement from that render by what the ring held then.
    EXPECT(fixture.pump_until([&] { return source.requests.load() > requests_before_consuming; }));
    auto request = source.latest();
    auto frames_held_at_render = request.frames_supplied_before - frames_consumed_before_render;
    auto expected = rendered_at + AK::Duration::from_time_units(static_cast<i64>(frames_held_at_render), 1, FakeDeviceStream::SAMPLE_RATE);
    // The mixer reads its clock inside the render, after the test did, and the pump rounds to a frame. A pump starved
    // past what the ring held finds it empty and dates by its own clock, never later than the request's.
    EXPECT(request.buffer_starts_playing_at >= expected - AK::Duration::from_milliseconds(1));
    EXPECT(request.buffer_starts_playing_at <= max(expected + AK::Duration::from_milliseconds(20), request.requested_at));
}

TEST_CASE(resume_after_a_drain_plays_again)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.render_until_full_buffer_of(1.0f));
    bool drained = false;
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.render_until([&] { return drained; }));

    bool resumed = false;
    stream->resume()->when_resolved([&] { resumed = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.render_until_full_buffer_of(1.0f));
    EXPECT(fixture.pump_until([&] { return resumed; }));
}

TEST_CASE(stream_created_playing_starts_at_once)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Playing, source);
    EXPECT(fixture.pump_until([&] { return fixture.device->is_playing() && source.frames_supplied.load() > 0; }));
}

TEST_CASE(drain_completes_once_the_device_has_played_the_ring_out)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= 480; }));

    // Stop supplying, then drain: the request completes only after the fake has rendered everything.
    source.starved = true;
    bool drained = false;
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) {});
    fixture.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!drained);
    EXPECT(fixture.render_until([&] { return drained; }));
    EXPECT(fixture.pump_until([&] { return !fixture.device->is_playing(); }));
}

TEST_CASE(discard_drops_the_ring_and_suspends)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= 480; }));

    bool discarded = false;
    stream->discard_buffer_and_suspend()->when_resolved([&] { discarded = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.pump_until([&] { return discarded && !fixture.device->is_playing(); }));
    auto requests_after_discard = source.requests.load();

    // Nothing plays and the pump has stopped asking.
    auto output = fixture.device->render(480);
    for (auto sample : output)
        EXPECT_EQ(sample, 0.0f);
    fixture.pump_until([&] { return false; }, AK::Duration::from_milliseconds(100));
    EXPECT_EQ(source.requests.load(), requests_after_discard);
}

TEST_CASE(a_starved_stream_waits_for_notify_data_available)
{
    Source source;
    RemoteFixture fixture;
    source.starved = true;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.requests.load() >= 1; }));
    auto requests_while_starved = source.requests.load();

    // The pump parks after the empty return: no further requests over several periods.
    fixture.pump_until([&] { return false; }, AK::Duration::from_milliseconds(150));
    EXPECT_EQ(source.requests.load(), requests_while_starved);

    source.starved = false;
    stream->notify_data_available();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() > 0; }));
}

TEST_CASE(a_notification_during_a_starved_request_is_not_lost)
{
    Source source;
    source.starved = true;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    // Data arrives while the pump is still inside the request that finds nothing: the pump must ask again.
    source.on_starved_request = [&] {
        source.starved = false;
        stream->notify_data_available();
    };
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() > 0; }));
}

TEST_CASE(volume_reaches_the_mix)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->set_volume(0.25);
    (void)stream->resume();
    EXPECT(fixture.render_until_full_buffer_of(0.25f));
}

TEST_CASE(losing_the_connection_reports_output_lost_and_settles_requests)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= 480; }));

    bool output_lost = false;
    stream->on_output_lost = [&] { output_lost = true; };

    // Dropping the server end closes the transport; the client notices and tells its streams.
    fixture.server = nullptr;
    EXPECT(fixture.pump_until([&] { return output_lost; }));

    bool drained = false;
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.pump_until([&] { return drained; }));
    auto requests_at_loss = source.requests.load();
    fixture.pump_until([&] { return false; }, AK::Duration::from_milliseconds(50));
    EXPECT_EQ(source.requests.load(), requests_at_loss);
}

TEST_CASE(destroying_a_stream_removes_it_from_the_mixer)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return fixture.mixer->has_clients(); }));
    stream = nullptr;
    EXPECT(fixture.pump_until([&] { return !fixture.mixer->has_clients() && !fixture.device->is_playing(); }));
}
