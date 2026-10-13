/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FakeDeviceStream.h"
#include "FakeRecordStream.h"
#include <AK/Atomic.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <AudioServer/CaptureDevices.h>
#include <AudioServer/PlaybackStreamMixer.h>
#include <AudioServer/ServerConnection.h>
#include <LibAudio/ClientConnection.h>
#include <LibAudio/RemoteRecordStream.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>

// Where a stream's frames land. Written on the pump thread, read from the test. Declared before the fixture in every
// test, since the pump may still be delivering when a test drops its stream.
struct Sink {
    Atomic<u64> frames { 0 };
    Atomic<u32> sample_rate { 0 };
    Atomic<u32> channel_count { 0 };
    Atomic<bool> every_sample_matched { true };
    float expected_value { 0 };

    Audio::RecordStream::RecordCallback callback()
    {
        return [this](ReadonlyBytes bytes, Audio::SampleSpecification const& specification) {
            sample_rate = specification.sample_rate();
            channel_count = specification.channel_count();
            ReadonlySpan<float> samples { reinterpret_cast<float const*>(bytes.data()), bytes.size() / sizeof(float) };
            for (auto sample : samples) {
                if (sample != expected_value)
                    every_sample_matched = false;
            }
            frames += samples.size() / specification.channel_count();
        };
    }
};

// A server connection with a mixer on a fake device and capture devices from a fake factory, and a client connection
// on the other end of a paired transport, all on one event loop.
struct RemoteCaptureFixture {
    Core::EventLoop loop;
    Vector<RefPtr<FakeRecordStream>> opened;
    RefPtr<Audio::PlaybackStreamMixer> mixer;
    RefPtr<Audio::CaptureDevices> capture_devices;
    RefPtr<Audio::ServerConnection> server;
    WeakPtr<Audio::ServerConnection> server_weak;
    RefPtr<Audio::ClientConnection> client;
    WeakPtr<Audio::ClientConnection> client_weak;
    bool server_died { false };
    // The fixture stands in for the Browser: the devices it was asked about, and its answer. An answer withheld waits
    // in pending_capture_decisions for the test to give.
    Vector<ByteString> capture_requests;
    bool capture_permission_answer { true };
    bool answer_capture_requests_at_once { true };
    Vector<Function<void(bool)>> pending_capture_decisions;

    RemoteCaptureFixture()
    {
        mixer = Audio::PlaybackStreamMixer::create(loop, 100, [](Audio::OutputState state, u32, Audio::PlaybackStream::AudioDataRequestCallback callback) {
            auto promise = Audio::PlaybackStream::CreatePromise::construct();
            promise->resolve(make_ref_counted<FakeDeviceStream>(state, move(callback)));
            return promise;
        });
        capture_devices = Audio::CaptureDevices::create([this](Audio::SampleSpecification const&, u32, StringView device_id, Audio::RecordStream::RecordCallback callback) {
            if (device_id == "missing"sv)
                return Audio::RecordStream::CreatePromise::rejected(Error::from_string_literal("No such device"));
            auto stream = make_ref_counted<FakeRecordStream>(Audio::SampleSpecification(FakeRecordStream::SAMPLE_RATE, Audio::ChannelMap::stereo()), move(callback));
            opened.append(stream);
            return Audio::RecordStream::CreatePromise::resolved(NonnullRefPtr<Audio::RecordStream>(stream));
        });
        auto paired = MUST(IPC::Transport::create_paired());
        server = Audio::ServerConnection::construct(move(paired.local), 1, *mixer, Audio::ServerConnection::DeviceEnumeration::None, *capture_devices, [this](ByteString const& device_id, Function<void(bool)> on_decided) {
            capture_requests.append(device_id);
            if (answer_capture_requests_at_once)
                on_decided(capture_permission_answer);
            else
                pending_capture_decisions.append(move(on_decided));
        });
        server->set_capture_permitted(true);
        server->on_death = [this] { server_died = true; };
        server_weak = server->make_weak_ptr<Audio::ServerConnection>();
        client = adopt_ref(*new Audio::ClientConnection(MUST(paired.remote_handle.create_transport())));
        client_weak = client->make_weak_ptr<Audio::ClientConnection>();
    }

    // The client goes first, and the loop runs until both ends are gone for good: a stream's last reference may be the
    // pump's, whose goodbye is deferred on this loop, and the server's shutdown is deferred with a reference of its own.
    ~RemoteCaptureFixture()
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

    RefPtr<Audio::RecordStream> create_record_stream(StringView device_id, Sink& sink, bool& rejected)
    {
        RefPtr<Audio::RecordStream> stream;
        auto promise = client->create_record_stream(device_id, sink.callback());
        promise->when_resolved([&](NonnullRefPtr<Audio::RecordStream>& created) { stream = created; });
        promise->when_rejected([&](Error&) { rejected = true; });
        EXPECT(pump_until([&] { return stream || rejected; }));
        return stream;
    }
};

TEST_CASE(a_record_stream_delivers_the_devices_frames_in_the_devices_format)
{
    Sink sink;
    sink.expected_value = 0.25f;
    RemoteCaptureFixture fixture;
    bool rejected = false;
    auto stream = fixture.create_record_stream("mic"sv, sink, rejected);
    EXPECT(!rejected);
    EXPECT_EQ(fixture.capture_requests, Vector<ByteString> { "mic" });
    EXPECT_EQ(fixture.opened.size(), 1u);
    EXPECT_EQ(stream->sample_specification().sample_rate(), FakeRecordStream::SAMPLE_RATE);
    EXPECT_EQ(stream->sample_specification().channel_count(), FakeRecordStream::CHANNEL_COUNT);

    fixture.opened[0]->capture(480, 0.25f);
    EXPECT(fixture.pump_until([&] { return sink.frames.load() >= 480; }));
    EXPECT_EQ(sink.frames.load(), 480u);
    EXPECT_EQ(sink.sample_rate.load(), FakeRecordStream::SAMPLE_RATE);
    EXPECT_EQ(sink.channel_count.load(), FakeRecordStream::CHANNEL_COUNT);
    EXPECT(sink.every_sample_matched.load());
}

TEST_CASE(streams_on_one_device_share_the_servers_stream_and_ring)
{
    Sink first_sink;
    Sink second_sink;
    first_sink.expected_value = 1.0f;
    second_sink.expected_value = 1.0f;
    RemoteCaptureFixture fixture;
    bool rejected = false;
    auto first = fixture.create_record_stream("mic"sv, first_sink, rejected);
    auto second = fixture.create_record_stream("mic"sv, second_sink, rejected);
    EXPECT(!rejected);
    EXPECT_EQ(fixture.opened.size(), 1u);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 1u);

    fixture.opened[0]->capture(100, 1.0f);
    EXPECT(fixture.pump_until([&] { return first_sink.frames.load() >= 100 && second_sink.frames.load() >= 100; }));

    // The device stays open for the stream that remains, which alone receives what follows.
    first = nullptr;
    fixture.pump_until([] { return false; }, AK::Duration::from_milliseconds(50));
    fixture.opened[0]->capture(100, 1.0f);
    EXPECT(fixture.pump_until([&] { return second_sink.frames.load() >= 200; }));
    EXPECT_EQ(first_sink.frames.load(), 100u);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 1u);

    second = nullptr;
    EXPECT(fixture.pump_until([&] { return fixture.capture_devices->open_device_count() == 0; }));
    fixture.opened.clear();
    EXPECT_EQ(FakeRecordStream::live_count.load(), 0u);
}

TEST_CASE(different_devices_get_different_streams)
{
    Sink first_sink;
    Sink second_sink;
    RemoteCaptureFixture fixture;
    bool rejected = false;
    auto first = fixture.create_record_stream("mic"sv, first_sink, rejected);
    auto second = fixture.create_record_stream("line-in"sv, second_sink, rejected);
    EXPECT(!rejected);
    EXPECT_EQ(fixture.opened.size(), 2u);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 2u);
}

TEST_CASE(a_device_that_will_not_open_rejects_the_stream)
{
    Sink sink;
    RemoteCaptureFixture fixture;
    bool rejected = false;
    auto stream = fixture.create_record_stream("missing"sv, sink, rejected);
    EXPECT(rejected);
    EXPECT(!stream);
    EXPECT(!fixture.server_died);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 0u);
}

TEST_CASE(a_request_the_browser_refuses_is_rejected)
{
    Sink sink;
    RemoteCaptureFixture fixture;
    fixture.capture_permission_answer = false;
    bool rejected = false;
    auto stream = fixture.create_record_stream("mic"sv, sink, rejected);
    EXPECT(rejected);
    EXPECT(!stream);
    EXPECT(!fixture.server_died);
    EXPECT_EQ(fixture.capture_requests.size(), 1u);
    EXPECT_EQ(fixture.opened.size(), 0u);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 0u);
}

TEST_CASE(losing_the_permission_closes_the_streams)
{
    Sink sink;
    RemoteCaptureFixture fixture;
    bool rejected = false;
    auto stream = fixture.create_record_stream("mic"sv, sink, rejected);
    EXPECT(!rejected);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 1u);

    bool revoked = false;
    bool lost = false;
    stream->on_capture_revoked = [&] { revoked = true; };
    stream->on_capture_lost = [&] { lost = true; };
    fixture.server->set_capture_permitted(false);
    EXPECT(fixture.pump_until([&] { return revoked; }));
    EXPECT(!lost);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 0u);

    // A new request asks the Browser afresh rather than reuse what was closed.
    fixture.server->set_capture_permitted(true);
    auto again = fixture.create_record_stream("mic"sv, sink, rejected);
    EXPECT(!rejected);
    EXPECT(again);
    EXPECT_EQ(fixture.capture_requests.size(), 2u);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 1u);
}

TEST_CASE(losing_the_permission_while_the_browser_is_asked_rejects_the_request)
{
    Sink sink;
    RemoteCaptureFixture fixture;
    fixture.answer_capture_requests_at_once = false;
    RefPtr<Audio::RecordStream> stream;
    bool rejected = false;
    auto promise = fixture.client->create_record_stream("mic"sv, sink.callback());
    promise->when_resolved([&](NonnullRefPtr<Audio::RecordStream>& created) { stream = created; });
    promise->when_rejected([&](Error&) { rejected = true; });
    EXPECT(fixture.pump_until([&] { return fixture.pending_capture_decisions.size() == 1; }));

    fixture.server->set_capture_permitted(false);
    EXPECT(fixture.pump_until([&] { return rejected; }));

    // The answer that comes late opens nothing.
    fixture.pending_capture_decisions[0](true);
    fixture.pump_until([] { return false; }, AK::Duration::from_milliseconds(10));
    EXPECT(!stream);
    EXPECT_EQ(fixture.opened.size(), 0u);
    EXPECT_EQ(fixture.capture_devices->open_device_count(), 0u);
}

TEST_CASE(losing_the_connection_reports_capture_lost)
{
    Sink sink;
    RemoteCaptureFixture fixture;
    bool rejected = false;
    auto stream = fixture.create_record_stream("mic"sv, sink, rejected);
    EXPECT(!rejected);

    bool lost = false;
    stream->on_capture_lost = [&] { lost = true; };
    fixture.server = nullptr;
    EXPECT(fixture.pump_until([&] { return lost; }));
}
