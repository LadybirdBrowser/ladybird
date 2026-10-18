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
#include <AudioServer/CaptureDevices.h>
#include <AudioServer/PlaybackStreamMixer.h>
#include <AudioServer/ServerConnection.h>
#include <LibAudio/AudioDevices.h>
#include <LibAudio/ClientConnection.h>
#include <LibAudio/RemotePlaybackStream.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibMedia/Sinks/AudioPlaybackSink.h>
#include <LibTest/TestCase.h>
#include <math.h>

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
        server = Audio::ServerConnection::construct(move(paired.local), 1, *mixer, Audio::ServerConnection::DeviceEnumeration::Platform, Audio::CaptureDevices::create([](Audio::SampleSpecification const&, u32, StringView, Audio::RecordStream::RecordCallback) { return Audio::RecordStream::CreatePromise::rejected(Error::from_string_literal("No capture in this test")); }), [](ByteString const&, Function<void(bool)> on_decided) { on_decided(false); });
        server->set_capture_permitted(true);
        server->on_death = [this] { server_died = true; };
        server_weak = server->make_weak_ptr<Audio::ServerConnection>();
        client = adopt_ref(*new Audio::ClientConnection(MUST(paired.remote_handle.create_transport())));
        client_weak = client->make_weak_ptr<Audio::ClientConnection>();
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
    stream->resume()->when_resolved([&] { resumed = true; }).when_rejected([](Error&&) { });
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
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) { });
    EXPECT(fixture.render_until([&] { return drained; }));

    bool resumed = false;
    stream->resume()->when_resolved([&] { resumed = true; }).when_rejected([](Error&&) { });
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
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) { });
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
    stream->discard_buffer_and_suspend()->when_resolved([&] { discarded = true; }).when_rejected([](Error&&) { });
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

TEST_CASE(a_volume_outside_the_unit_range_disconnects_the_client)
{
    for (auto volume : { -0.5f, 1.5f, NAN }) {
        Source source;
        RemoteFixture fixture;
        auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
        fixture.client->async_set_stream_volume(static_cast<Audio::RemotePlaybackStream&>(*stream).stream_id(), volume);
        EXPECT(fixture.pump_until([&] { return fixture.server_died; }));
    }
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
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) { });
    EXPECT(fixture.pump_until([&] { return drained; }));
    auto requests_at_loss = source.requests.load();
    fixture.pump_until([&] { return false; }, AK::Duration::from_milliseconds(50));
    EXPECT_EQ(source.requests.load(), requests_at_loss);
}

TEST_CASE(watching_devices_reports_the_servers_list_at_once)
{
    RemoteFixture fixture;
    // The test stands in for the platform's device watch.
    auto& devices = Audio::AudioDevices::the();
    devices.report_device_list(Audio::AudioDeviceEnumeration {});

    Optional<Audio::AudioDeviceEnumeration> enumeration;
    fixture.client->watch_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> result) { enumeration = result.release_value(); });
    EXPECT(fixture.pump_until([&] { return enumeration.has_value(); }));
    EXPECT_EQ(enumeration->inputs.size(), devices.input_devices().size());
    EXPECT_EQ(enumeration->outputs.size(), devices.output_devices().size());
}

TEST_CASE(a_device_change_on_the_server_reaches_a_watching_client)
{
    // The watcher may still be told of a change while the fixture shuts the client down.
    size_t reports = 0;
    Vector<Audio::AudioDeviceInfo> last_inputs;
    RemoteFixture fixture;
    // The test stands in for the platform's device watch.
    auto& devices = Audio::AudioDevices::the();
    devices.report_device_list(Audio::AudioDeviceEnumeration {});
    Audio::AudioDeviceEnumeration original { .inputs = devices.input_devices(), .outputs = devices.output_devices() };

    fixture.client->watch_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> result) {
        reports++;
        last_inputs = result.release_value().inputs;
    });
    EXPECT(fixture.pump_until([&] { return reports == 1; }));

    // The source reports a new device, and the client hears the list again.
    Audio::AudioDeviceInfo microphone { .dom_device_id = "test:input:1", .label = "Test microphone", .group_id = {}, .sample_rate_hz = 48000, .channel_count = 1, .is_default = true };
    devices.report_device_list(Audio::AudioDeviceEnumeration { .inputs = { microphone }, .outputs = {} });
    EXPECT(fixture.pump_until([&] { return reports == 2; }));
    EXPECT_EQ(last_inputs.size(), 1u);
    EXPECT_EQ(last_inputs[0].label, "Test microphone"sv);

    devices.report_device_list(move(original));
}

TEST_CASE(without_the_permission_a_client_learns_only_that_devices_exist)
{
    // The watcher may still be told of a change while the fixture shuts the client down.
    size_t reports = 0;
    Audio::AudioDeviceEnumeration last_report;
    RemoteFixture fixture;
    // The test stands in for the platform's device watch.
    auto& devices = Audio::AudioDevices::the();
    devices.report_device_list(Audio::AudioDeviceEnumeration {});
    Audio::AudioDeviceEnumeration original { .inputs = devices.input_devices(), .outputs = devices.output_devices() };
    Audio::AudioDeviceInfo microphone { .dom_device_id = "test:input:1", .label = "Test microphone", .group_id = "test:group", .sample_rate_hz = 48000, .channel_count = 1, .is_default = true };
    Audio::AudioDeviceInfo speaker { .dom_device_id = "test:output:1", .label = "Test speaker", .group_id = "test:group", .sample_rate_hz = 48000, .channel_count = 2, .is_default = false };
    devices.report_device_list(Audio::AudioDeviceEnumeration { .inputs = { microphone }, .outputs = { speaker, speaker } });

    auto expect_nameless = [&](Vector<Audio::AudioDeviceInfo> const& reported) {
        EXPECT_EQ(reported.size(), 1u);
        EXPECT(reported[0].dom_device_id.is_empty());
        EXPECT(reported[0].label.is_empty());
        EXPECT(reported[0].group_id.is_empty());
        EXPECT(reported[0].is_default);
    };
    fixture.server->set_capture_permitted(false);
    fixture.client->watch_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> result) {
        reports++;
        last_report = result.release_value();
    });
    EXPECT(fixture.pump_until([&] { return reports == 1; }));
    expect_nameless(last_report.inputs);
    expect_nameless(last_report.outputs);

    // The permission brings the names at once, and losing it takes them away again.
    fixture.server->set_capture_permitted(true);
    EXPECT(fixture.pump_until([&] { return reports == 2; }));
    EXPECT_EQ(last_report.inputs.size(), 1u);
    EXPECT_EQ(last_report.inputs[0].label, "Test microphone"sv);
    EXPECT_EQ(last_report.outputs.size(), 2u);
    EXPECT_EQ(last_report.outputs[0].label, "Test speaker"sv);
    fixture.server->set_capture_permitted(false);
    EXPECT(fixture.pump_until([&] { return reports == 3; }));
    expect_nameless(last_report.inputs);
    expect_nameless(last_report.outputs);

    devices.report_device_list(move(original));
}

TEST_CASE(losing_the_connection_reports_an_error_to_the_watcher)
{
    RemoteFixture fixture;
    bool errored = false;
    fixture.client->watch_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> result) {
        if (result.is_error())
            errored = true;
    });

    fixture.server = nullptr;
    EXPECT(fixture.pump_until([&] { return errored; }));
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

// A mixer on a fake device whose server connections are handed out through the process-wide transport factory, the
// way a sink in MediaServer reaches the AudioServer. Each request gets a fresh server end on the same mixer.
struct RemoteSinkFixture {
    Core::EventLoop loop;
    RefPtr<FakeDeviceStream> device;
    RefPtr<Audio::PlaybackStreamMixer> mixer;
    Vector<NonnullRefPtr<Audio::ServerConnection>> servers;
    size_t connections_requested { 0 };
    bool accepting_connections { true };

    RemoteSinkFixture()
    {
        mixer = Audio::PlaybackStreamMixer::create(loop, 100, [this](Audio::OutputState state, u32, Audio::PlaybackStream::AudioDataRequestCallback callback) {
            device = make_ref_counted<FakeDeviceStream>(state, move(callback));
            auto promise = Audio::PlaybackStream::CreatePromise::construct();
            promise->resolve(*device);
            return promise;
        });
        Audio::ClientConnection::set_transport_factory([this]() -> ErrorOr<NonnullOwnPtr<IPC::Transport>> {
            connections_requested++;
            if (!accepting_connections)
                return Error::from_string_literal("The fixture is no longer accepting connections");
            auto paired = TRY(IPC::Transport::create_paired());
            servers.append(Audio::ServerConnection::construct(move(paired.local), static_cast<int>(connections_requested), *mixer, Audio::ServerConnection::DeviceEnumeration::Platform, Audio::CaptureDevices::create([](Audio::SampleSpecification const&, u32, StringView, Audio::RecordStream::RecordCallback) { return Audio::RecordStream::CreatePromise::rejected(Error::from_string_literal("No capture in this test")); }), [](ByteString const&, Function<void(bool)> on_decided) { on_decided(false); }));
            auto transport = TRY(paired.remote_handle.create_transport());
            return transport;
        });
    }

    ~RemoteSinkFixture()
    {
        Audio::ClientConnection::set_transport_factory({});
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

    // Closing every server end makes the client connection die. Done with the sink still alive, so that its reopen
    // asks for a connection and is refused: the sign that the client is gone, before the loop is.
    void close_connections_and_wait_for_the_client_to_die()
    {
        accepting_connections = false;
        auto requests_before = connections_requested;
        servers.clear();
        EXPECT(pump_until([&] { return connections_requested > requests_before; }));
        pump_until([] { return false; }, AK::Duration::from_milliseconds(10));
    }
};

TEST_CASE(audio_playback_sink_reopens_its_output_after_loss_with_a_seek_in_flight)
{
    RemoteSinkFixture fixture;
    auto remote_output_factory = Media::AudioOutputFactory::create();
    RefPtr<Media::AudioPlaybackSink> sink = MUST(Media::AudioPlaybackSink::try_create([](Media::PipelineStatus) { }, remote_output_factory));
    sink->start();
    sink->resume();
    EXPECT(fixture.pump_until([&] { return fixture.mixer->active_client_count() == 1; }));
    EXPECT_EQ(fixture.connections_requested, 1u);

    // The server goes away with the seek's request unanswered: the sink hears of the loss, then of the request's fate.
    sink->seek(AK::Duration::from_seconds(1));
    fixture.servers.clear();
    EXPECT(fixture.pump_until([&] { return fixture.connections_requested == 2; }));

    // The new stream seeks to the pending target and plays on.
    EXPECT(fixture.pump_until([&] { return fixture.mixer->active_client_count() == 1 && fixture.servers.size() == 1; }));
    EXPECT(fixture.pump_until([&] { return sink->time_reader().current_time() >= AK::Duration::from_seconds(1); }));

    fixture.close_connections_and_wait_for_the_client_to_die();
    sink = nullptr;
    fixture.pump_until([] { return false; }, AK::Duration::from_milliseconds(10));
}
