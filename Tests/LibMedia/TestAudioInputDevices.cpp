/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestSuite.h>

#ifndef AK_OS_WINDOWS
#    include <AK/Format.h>
#    include <AK/LexicalPath.h>
#    include <AK/ScopeGuard.h>
#    include <AK/StringView.h>
#    include <AK/Vector.h>
#    include <AudioServer/AudioInputRingStream.h>
#    include <AudioServer/AudioInputStreamDescriptor.h>
#    include <LibAudioServerClient/Client.h>
#    include <LibCore/Environment.h>
#    include <LibCore/EventLoop.h>
#    include <LibCore/Process.h>
#    include <LibCore/Socket.h>
#    include <LibCore/System.h>
#    include <LibIPC/Transport.h>
#    include <LibMedia/MediaCapture/AudioInputDevices.h>
#    include <signal.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

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

    return Error::from_string_literal("Failed to locate AudioServer executable in build outputs");
}

static ErrorOr<Core::Process> spawn_audioserver_with_takeover(int takeover_fd)
{
    auto audio_server_path = TRY(find_audioserver_executable_path());

    auto takeover_string = ByteString::formatted("smoke:{}", takeover_fd);
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

#ifndef AK_OS_WINDOWS
TEST_CASE(media_capture_audio_input_devices_enumerate)
{
    Core::EventLoop event_loop;

    int fds[2] {};
    EXPECT(!Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds).is_error());

    auto audio_server_process_or_error = spawn_audioserver_with_takeover(fds[1]);
    EXPECT(!audio_server_process_or_error.is_error());
    if (audio_server_process_or_error.is_error()) {
        (void)Core::System::close(fds[0]);
        (void)Core::System::close(fds[1]);
        return;
    }

    auto audio_server_process = audio_server_process_or_error.release_value();

    auto kill_audio_server = AK::ArmedScopeGuard([&] {
        (void)Core::System::kill(audio_server_process.pid(), SIGTERM);
        (void)audio_server_process.wait_for_termination();
    });

    EXPECT(!Core::System::close(fds[1]).is_error());

    auto socket_or_error = Core::LocalSocket::adopt_fd(fds[0]);
    EXPECT(!socket_or_error.is_error());
    if (socket_or_error.is_error())
        return;

    auto transport = make<IPC::Transport>(socket_or_error.release_value());
    auto client = adopt_ref(*new AudioServerClient::Client(move(transport)));
    AudioServerClient::Client::set_default_client(client);

    auto devices_or_error = Media::Capture::AudioInputDevices::enumerate();
    EXPECT(!devices_or_error.is_error());
    if (devices_or_error.is_error())
        return;

    auto devices = devices_or_error.release_value();
    size_t default_count = 0;
    for (auto const& device : devices) {
        dbgln("Audio input device: id={} label={} persistent_id={} sample_rate={}Hz channels={} default={}",
            device.device_id,
            device.label,
            device.persistent_id,
            device.sample_rate_hz,
            device.channel_count,
            device.is_default);
        EXPECT(device.device_id != 0);
        EXPECT(device.channel_count > 0);
        if (device.is_default)
            ++default_count;
    }

    EXPECT(default_count <= 1);

    if (devices.is_empty()) {
        kill_audio_server.disarm();
        EXPECT(!Core::System::kill(audio_server_process.pid(), SIGTERM).is_error());
        EXPECT(audio_server_process.wait_for_termination().is_error() == false);
        return;
    }

    auto const& candidate = devices.first();
    auto stream_or_error = client->create_audio_input_stream(candidate.device_id, 0, 0, 4096, AudioServer::StreamOverflowPolicy::DropOldest);
    if (stream_or_error.is_error()) {
        kill_audio_server.disarm();
        EXPECT(!Core::System::kill(audio_server_process.pid(), SIGTERM).is_error());
        EXPECT(audio_server_process.wait_for_termination().is_error() == false);
        return;
    }

    auto stream_descriptor = stream_or_error.release_value();
    auto* header = stream_descriptor.shared_memory.data<AudioServer::RingStreamHeader>();
    EXPECT(header != nullptr);
    if (!header) {
        (void)client->destroy_audio_input_stream(stream_descriptor.stream_id);
        kill_audio_server.disarm();
        EXPECT(!Core::System::kill(audio_server_process.pid(), SIGTERM).is_error());
        EXPECT(audio_server_process.wait_for_termination().is_error() == false);
        return;
    }

    u64 initial_write = AudioServer::ring_stream_load_write_frame(*header);
    bool advanced = false;
    auto deadline = AK::MonotonicTime::now() + AK::Duration::from_milliseconds(500);
    while (AK::MonotonicTime::now() < deadline) {
        (void)Core::System::sleep_ms(10);
        u64 now_write = AudioServer::ring_stream_load_write_frame(*header);
        if (now_write != initial_write) {
            advanced = true;
            break;
        }
    }

    EXPECT(advanced);
    (void)client->destroy_audio_input_stream(stream_descriptor.stream_id);

    kill_audio_server.disarm();
    EXPECT(!Core::System::kill(audio_server_process.pid(), SIGTERM).is_error());
    EXPECT(audio_server_process.wait_for_termination().is_error() == false);
}
#else
TEST_CASE(media_capture_audio_input_devices_enumerate)
{
    EXPECT(true);
}
#endif
