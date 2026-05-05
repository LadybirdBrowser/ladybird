/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AudioServerExampleHelper.h"

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibAudioServer/BrokerOfAudioServer.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibAudioServer/SingleSinkSessionClient.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>

// This example sends a square wave to AudioServer for 5 seconds.
//
// The broker process (the UI/browser launches AudioServer
// early and keeps an IPC connection to it. When a child helper process (like
// WebContent, WebAudioWorker, etc.) needs an AudioServer connection, the broker
// asks AudioServer to create a fresh client socket then passes it back to
// the child process.
//
// See LibWebView:
// - Application::launch_audio_server() starts AudioServer and caches the client.
// - connect_new_audio_server_client() sends ConnectNewClients(1) and returns
//   an IPC::File holding the new socket.

ErrorOr<int> ladybird_main(Main::Arguments)
{
    Core::EventLoop event_loop;
    static constexpr u32 target_latency_ms = 50;

    ByteString server_name = ByteString::formatted("minimal-audioserver-example.{}", Core::System::getpid());
    SpawnedAudioServerForExample audio_server = TRY(launch_audioserver_for_example(server_name));

    auto kill_audio_server = AK::ArmedScopeGuard([&] {
        (void)Core::System::kill(audio_server.process.pid(), SIGTERM);
        (void)audio_server.process.wait_for_termination();
    });

    auto response = TRY(audio_server.broker->connect_new_client("*"sv, "*"sv, true));
    auto session_client = TRY(create_session_client_for_example(response.handle));
    auto client = TRY(Audio::SingleSinkSessionClient::try_create(session_client));

    // Request one output sink. AudioServer returns a shared memory ring buffer
    // asynchronously, and we write interleaved f32 frames into it.
    Optional<Audio::OutputSink> maybe_session;
    Optional<ByteString> maybe_session_error;

    TRY(client->request_output_sink(
        [&](Audio::OutputSink const& session) {
            maybe_session = session;
        },
        [&](u64, ByteString const& error) {
            maybe_session_error = error;
        },
        0,
        target_latency_ms));

    event_loop.spin_until([&] {
        return maybe_session.has_value() || maybe_session_error.has_value();
    });

    if (maybe_session_error.has_value())
        return Error::from_string_literal("MinimalAudioServerClient: async audio output session creation failed");

    auto session = maybe_session.release_value();

    // Ring buffer concept:
    // - It's a single-producer, single-consumer (SPSC) circular buffer.
    //   The client is the producer (writes samples). AudioServer is the consumer
    //   (reads samples and mixes them into the output device).
    // - The backing storage is shared memory. AudioServer allocates an anonymous
    //   shared buffer and maps it into both processes. The client does not send
    //   PCM over IPC; it just writes into the shared mapping.
    // - Synchronization is done with atomic read/write positions inside the
    //   ring implementation. There is no explicit lock in the common case.
    //   available_to_write() and try_write() coordinate with the consumer using
    //   those atomics and the required memory barriers.
    // - If the producer falls behind, AudioServer will read less (or silence)
    //   for that session for that device callback. If the producer gets too far
    //   ahead, try_write() will return 0 and we wait.

    u32 sample_rate = session.sample_rate;
    u32 channel_count = session.channel_count;

    if (sample_rate == 0 || channel_count == 0)
        return Error::from_string_literal("MinimalAudioServerClient: invalid output format");

    outln("MinimalAudioServerClient: format {} Hz, {} channels", sample_rate, channel_count);

    double const frequency_hz = 440.0;
    float const amplitude = 0.12f;
    double phase_cycles = 0.0;

    u64 total_frames_to_write = static_cast<u64>(sample_rate) * 2u;
    static constexpr u32 max_frames_per_write = 512;

    while (total_frames_to_write > 0) {
        u32 frames_this_write = max_frames_per_write;
        if (static_cast<u64>(frames_this_write) > total_frames_to_write)
            frames_this_write = static_cast<u32>(total_frames_to_write);

        size_t bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(float);
        size_t bytes_to_write = static_cast<size_t>(frames_this_write) * bytes_per_frame;

        if (session.ring.available_to_write() < bytes_to_write) {
            usleep(1000);
            continue;
        }

        Vector<float> interleaved;
        interleaved.resize(static_cast<size_t>(frames_this_write) * static_cast<size_t>(channel_count));

        for (u32 frame = 0; frame < frames_this_write; ++frame) {
            phase_cycles += frequency_hz / static_cast<double>(sample_rate);
            if (phase_cycles >= 1.0)
                phase_cycles -= 1.0;
            float sample_value = (phase_cycles < 0.5) ? amplitude : -amplitude;
            for (u32 ch = 0; ch < channel_count; ++ch)
                interleaved[(static_cast<size_t>(frame) * channel_count) + static_cast<size_t>(ch)] = sample_value;
        }

        ReadonlyBytes bytes { reinterpret_cast<u8 const*>(interleaved.data()), interleaved.size() * sizeof(float) };
        size_t written = 0;
        while (written < bytes.size()) {
            size_t did_write = session.ring.try_write(bytes.slice(written));
            written += did_write;
            if (did_write == 0)
                usleep(1000);
        }

        total_frames_to_write -= frames_this_write;
    }

    kill_audio_server.disarm();
    TRY(Core::System::kill(audio_server.process.pid(), SIGTERM));
    TRY(audio_server.process.wait_for_termination());

    outln("MinimalAudioServerClient: done");
    return 0;
}
