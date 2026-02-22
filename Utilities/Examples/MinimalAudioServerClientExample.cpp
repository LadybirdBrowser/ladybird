/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibAudioServer/BrokerOfAudioServer.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
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

static ErrorOr<ByteString> find_audioserver_executable_path();
static ErrorOr<Core::Process> spawn_audioserver_with_takeover(int takeover_fd);
static ErrorOr<NonnullRefPtr<AudioServer::BrokerOfAudioServer>> create_broker_client_from_fd(int fd);
static ErrorOr<NonnullRefPtr<AudioServer::SessionClientOfAudioServer>> create_client_from_fd(int fd);

ErrorOr<int> ladybird_main(Main::Arguments)
{
    Core::EventLoop event_loop;
    static constexpr u32 target_latency_ms = 50;

    // Test/example only: create a socketpair, then give one end to AudioServer
    // via SOCKET_TAKEOVER.
    int fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto audio_server_process = TRY(spawn_audioserver_with_takeover(fds[1]));

    auto kill_audio_server = AK::ArmedScopeGuard([&] {
        (void)Core::System::kill(audio_server_process.pid(), SIGTERM);
        (void)audio_server_process.wait_for_termination();
    });

    TRY(Core::System::close(fds[1]));

    auto broker_client = TRY(create_broker_client_from_fd(fds[0]));

    auto response = TRY(broker_client->connect_new_client("*"sv, "*"sv, true));
    auto client = TRY(create_client_from_fd(response.socket.take_fd()));

    // Create one audio output session. AudioServer returns a shared memory ring
    // buffer asynchronously, and we write interleaved f32 frames into it.
    Optional<AudioServer::OutputSink> maybe_session;
    Optional<ByteString> maybe_session_error;
    Optional<u64> requested_session_id;

    client->on_output_sink_ready = [&](AudioServer::OutputSink session) {
        if (requested_session_id.has_value() && session.session_id != requested_session_id.value())
            return;

        if (!requested_session_id.has_value())
            requested_session_id = session.session_id;
        maybe_session = move(session);
    };
    client->on_output_sink_failed = [&](u64 session_id, ByteString error) {
        if (requested_session_id.has_value() && session_id != requested_session_id.value())
            return;

        if (!requested_session_id.has_value())
            requested_session_id = session_id;
        maybe_session_error = move(error);
    };

    TRY(client->create_session(target_latency_ms, [&](u64 session_id) {
            if (!requested_session_id.has_value())
                requested_session_id = session_id; }, [&](ByteString error) { maybe_session_error = move(error); }));
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

    // Generate 2 seconds worth.
    u64 total_frames_to_write = static_cast<u64>(sample_rate) * 2u;
    static constexpr u32 max_frames_per_write = 512;

    while (total_frames_to_write > 0) {
        u32 frames_this_write = max_frames_per_write;
        if (static_cast<u64>(frames_this_write) > total_frames_to_write)
            frames_this_write = static_cast<u32>(total_frames_to_write);

        size_t bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(float);
        size_t bytes_to_write = static_cast<size_t>(frames_this_write) * bytes_per_frame;

        // Wait until there is enough space in the ring. In a real-time-ish client
        // you usually keep the ring topped up, rather than writing in big bursts.
        //
        // Note that this is not a "notify" style API. AudioServer continuously
        // drains the ring on the audio thread. We just keep writing ahead.
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
            // try_write() may write fewer bytes than requested if the ring is
            // close to full. We retry until the whole buffer is enqueued.
            size_t did_write = session.ring.try_write(bytes.slice(written));
            written += did_write;
            if (did_write == 0)
                usleep(1000);
        }

        total_frames_to_write -= frames_this_write;
    }

    kill_audio_server.disarm();
    TRY(Core::System::kill(audio_server_process.pid(), SIGTERM));
    TRY(audio_server_process.wait_for_termination());

    outln("MinimalAudioServerClient: done");
    return 0;
}

static ErrorOr<ByteString> find_audioserver_executable_path()
{
    auto current_executable_path = TRY(Core::System::current_executable_path());

    LexicalPath current_executable_lexical_path(current_executable_path);
    auto current_dir = current_executable_lexical_path.dirname();

    Vector<ByteString> candidates;

    candidates.append(LexicalPath::join(current_dir, "AudioServer"sv).string());
    candidates.append(LexicalPath::join(current_dir, "Ladybird.app"sv, "Contents"sv, "MacOS"sv, "AudioServer"sv).string());
    candidates.append(LexicalPath::join(current_dir, ".."sv, "libexec"sv, "AudioServer"sv).string());

    for (auto& candidate : candidates) {
        auto path = LexicalPath::canonicalized_path(move(candidate));
        if (!Core::System::access(path, X_OK).is_error())
            return ByteString(path);
    }

    return Error::from_string_literal("MinimalAudioServerClient: failed to locate AudioServer executable");
}

// In an actual Ladybird browser run, you would not use SOCKET_TAKEOVER.
// Helpers ask the broker for an AudioServer connection.

static ErrorOr<Core::Process> spawn_audioserver_with_takeover(int takeover_fd)
{
    // Test/example only: we set SOCKET_TAKEOVER so the AudioServer process will
    // reuse the already-open socketpair() fd.
    auto audio_server_path = TRY(find_audioserver_executable_path());

    auto takeover_string = ByteString::formatted("minimal-example:{}", takeover_fd);
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));

    Vector<ByteString> child_arguments;

    Core::ProcessSpawnOptions options {
        .name = "AudioServer"sv,
        .executable = audio_server_path,
        .search_for_executable_in_path = false,
        .arguments = child_arguments,
    };

    auto audio_server_process = TRY(Core::Process::spawn(options));
    TRY(Core::Environment::unset("SOCKET_TAKEOVER"sv));

    return audio_server_process;
}

static ErrorOr<NonnullRefPtr<AudioServer::SessionClientOfAudioServer>> create_client_from_fd(int fd)
{
    auto socket_or_error = Core::LocalSocket::adopt_fd(fd);
    if (socket_or_error.is_error())
        return socket_or_error.release_error();

    auto transport = make<IPC::Transport>(socket_or_error.release_value());
    return adopt_ref(*new AudioServer::SessionClientOfAudioServer(move(transport)));
}

static ErrorOr<NonnullRefPtr<AudioServer::BrokerOfAudioServer>> create_broker_client_from_fd(int fd)
{
    auto socket_or_error = Core::LocalSocket::adopt_fd(fd);
    if (socket_or_error.is_error())
        return socket_or_error.release_error();

    auto transport = make<IPC::Transport>(socket_or_error.release_value());
    return adopt_ref(*new AudioServer::BrokerOfAudioServer(move(transport)));
}
