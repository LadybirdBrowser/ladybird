/*
 * Copyright (c) 2023-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibAudioServer/BrokerOfAudioServer.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#else
#    include <LibIPC/TransportSocket.h>
#endif
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Sinks/AudioMixingSink.h>
#include <LibMedia/TrackType.h>
#include <LibTest/TestSuite.h>

static ErrorOr<ByteString> find_audioserver_executable_path();
struct BrokerBootstrap {
    int broker_fd { -1 };
    int child_takeover_fd { -1 };
};
static ErrorOr<BrokerBootstrap> create_broker_bootstrap();
static ErrorOr<void> close_child_bootstrap_fd(BrokerBootstrap& bootstrap);
static ErrorOr<Core::Process> spawn_audioserver(ByteString const& server_name, int takeover_fd = -1);
static ErrorOr<NonnullRefPtr<Audio::BrokerOfAudioServer>> create_broker_client(ByteString const& server_name, int fd = -1);
static ErrorOr<NonnullRefPtr<Audio::SessionClientOfAudioServer>> create_client_from_handle(IPC::TransportHandle const& handle);
static ErrorOr<void> init_audio_server_client(ByteString const& server_name, BrokerBootstrap& bootstrap);
static NonnullRefPtr<Media::IncrementallyPopulatedStream> load_test_file(StringView path);
static NonnullRefPtr<Media::Demuxer> create_demuxer(NonnullRefPtr<Media::IncrementallyPopulatedStream> const& stream);

TEST_CASE(create_and_destroy_audio_mixing_sink)
{
#if !defined(HAVE_PULSEAUDIO) && !defined(AK_OS_MACOS)
    return;
#endif
    Core::EventLoop event_loop;
    ByteString server_name = ByteString::formatted("test-audio-mixing-sink.{}", Core::System::getpid());
    auto bootstrap_result = create_broker_bootstrap();
    if (bootstrap_result.is_error()) {
        FAIL(ByteString::formatted("Failed to create test audio bootstrap: {}", bootstrap_result.error()));
        return;
    }
    BrokerBootstrap bootstrap = bootstrap_result.release_value();

    auto process_result = spawn_audioserver(server_name, bootstrap.child_takeover_fd);
    if (process_result.is_error()) {
        FAIL(ByteString::formatted("Failed to spawn AudioServer: {}", process_result.error()));
        return;
    }

    auto audio_server_process = process_result.release_value();
    auto cleanup_audio_server = AK::ArmedScopeGuard([&] {
        Audio::SessionClientOfAudioServer::set_default_client(nullptr);
        (void)Core::System::kill(audio_server_process.pid(), SIGTERM);
        (void)audio_server_process.wait_for_termination();
    });

    MUST(init_audio_server_client(server_name, bootstrap));

    {
        auto sink_result = Media::AudioMixingSink::try_create();
        if (sink_result.is_error()) {
            FAIL(ByteString::formatted("AudioMixingSink::try_create failed: {}", sink_result.error()));
            return;
        }

        auto sink = sink_result.release_value();
        EXPECT_EQ(sink->current_time(), AK::Duration::zero());

        auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
        auto demuxer = create_demuxer(stream);
        auto optional_track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
        VERIFY(optional_track.has_value());
        auto track = optional_track.release_value();

        auto provider = TRY_OR_FAIL(Media::AudioDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track));

        bool hit_error = false;

        provider->set_error_handler([&](Media::DecoderError&& error) {
            if (error.category() == Media::DecoderErrorCategory::EndOfStream)
                return;
            warnln("AudioDataProvider error: {}", error);
            hit_error = true;
        });

        sink->set_provider(track, provider);
        sink->resume();

        auto const timeout = AK::Duration::from_seconds(2);
        auto const start = MonotonicTime::now_coarse();

        bool current_time_advanced = sink->current_time() > AK::Duration::zero();
        while (!current_time_advanced && !hit_error) {
            if (MonotonicTime::now_coarse() - start >= timeout)
                break;
            event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            current_time_advanced = sink->current_time() > AK::Duration::zero();
        }

        if (hit_error)
            FAIL("Decoder reported a non-eos error while waiting for initial playback");
        if (!current_time_advanced)
            FAIL("Expected sink current_time() to advance above zero after decode started");

        auto advanced_time = sink->current_time();
        bool advanced_again = false;
        auto const advance_start = MonotonicTime::now_coarse();
        while (!advanced_again && !hit_error) {
            if (MonotonicTime::now_coarse() - advance_start >= timeout)
                break;
            event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            auto now = sink->current_time();
            if (now > advanced_time)
                advanced_again = true;
        }
        if (!advanced_again)
            FAIL("Expected sink current_time() to keep increasing while playing");

        sink->pause();

        auto paused_time = sink->current_time();
        auto const pause_window_start = MonotonicTime::now_coarse();
        while (!hit_error) {
            if (MonotonicTime::now_coarse() - pause_window_start >= AK::Duration::from_milliseconds(150))
                break;
            event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        auto paused_time_after_window = sink->current_time();
        auto paused_delta_ms = paused_time_after_window.to_milliseconds() - paused_time.to_milliseconds();
        if (paused_delta_ms > 25)
            FAIL(ByteString::formatted("Expected pause to stop time progression (delta <= 25ms), observed {}ms", paused_delta_ms));

        auto const seek_target = AK::Duration::from_milliseconds(250);
        sink->set_time(seek_target);
        auto seeked_time = sink->current_time();
        auto const seek_start = MonotonicTime::now_coarse();
        while (!hit_error) {
            if (MonotonicTime::now_coarse() - seek_start >= timeout)
                break;
            event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            seeked_time = sink->current_time();
            if (seeked_time.to_milliseconds() >= 200)
                break;
        }
        if (seeked_time.to_milliseconds() < 200)
            FAIL(ByteString::formatted("Expected seek target near 250ms (>=200ms tolerance), observed {}ms", seeked_time.to_milliseconds()));

        sink->resume();

        bool advanced_after_seek = false;
        auto const post_seek_start = MonotonicTime::now_coarse();
        while (!advanced_after_seek && !hit_error) {
            if (MonotonicTime::now_coarse() - post_seek_start >= timeout)
                break;
            event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            auto now = sink->current_time();
            if (now > seeked_time)
                advanced_after_seek = true;
        }
        if (!advanced_after_seek)
            FAIL("Expected sink current_time() to continue advancing after resume from seek");

        sink->pause();
        sink->set_provider(track, nullptr);
    }

    Audio::SessionClientOfAudioServer::set_default_client(nullptr);

    cleanup_audio_server.disarm();
    auto kill_result = Core::System::kill(audio_server_process.pid(), SIGTERM);
    if (kill_result.is_error())
        FAIL(ByteString::formatted("Failed to terminate AudioServer: {}", kill_result.error()));
    auto wait_result = audio_server_process.wait_for_termination();
    if (wait_result.is_error())
        FAIL(ByteString::formatted("Failed while waiting for AudioServer termination: {}", wait_result.error()));
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

    return Error::from_string_literal("Failed to locate AudioServer executable");
}

static ErrorOr<BrokerBootstrap> create_broker_bootstrap()
{
    BrokerBootstrap bootstrap;
#if !defined(AK_OS_MACOS)
    int fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
    bootstrap.broker_fd = fds[0];
    bootstrap.child_takeover_fd = fds[1];
#endif
    return bootstrap;
}

static ErrorOr<void> close_child_bootstrap_fd(BrokerBootstrap& bootstrap)
{
#if !defined(AK_OS_MACOS)
    if (bootstrap.child_takeover_fd != -1) {
        TRY(Core::System::close(bootstrap.child_takeover_fd));
        bootstrap.child_takeover_fd = -1;
    }
#else
    (void)bootstrap;
#endif
    return {};
}

static ErrorOr<Core::Process> spawn_audioserver(ByteString const& server_name, [[maybe_unused]] int takeover_fd)
{
    auto audio_server_path = TRY(find_audioserver_executable_path());
    Vector<ByteString> child_arguments;

#if defined(AK_OS_MACOS)
    child_arguments.append("--mach-server-name"sv);
    child_arguments.append(server_name);
#else
    (void)server_name;
    auto takeover_string = ByteString::formatted("test-audio-mixing-sink:{}", takeover_fd);
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));
#endif

    Core::ProcessSpawnOptions options {
        .name = "AudioServer"sv,
        .executable = audio_server_path,
        .search_for_executable_in_path = false,
        .arguments = child_arguments,
    };

    auto audio_server_process = TRY(Core::Process::spawn(options));
#if !defined(AK_OS_MACOS)
    TRY(Core::Environment::unset("SOCKET_TAKEOVER"sv));
#endif
    return audio_server_process;
}

static ErrorOr<NonnullRefPtr<Audio::BrokerOfAudioServer>> create_broker_client(ByteString const& server_name, [[maybe_unused]] int fd)
{
#if defined(AK_OS_MACOS)
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto transport_ports = IPC::bootstrap_transport_from_mach_server(server_name);
        if (!transport_ports.is_error())
            return adopt_ref(*new Audio::BrokerOfAudioServer(make<IPC::Transport>(move(transport_ports.value().receive_right), move(transport_ports.value().send_right))));
        usleep(10'000);
    }
    return Error::from_string_literal("Failed to connect to mach audio server");
#else
    (void)server_name;
    auto socket = TRY(Core::LocalSocket::adopt_fd(fd));
    auto transport = TRY(IPC::TransportSocket::from_socket(move(socket)));
    return adopt_ref(*new Audio::BrokerOfAudioServer(move(transport)));
#endif
}

static ErrorOr<NonnullRefPtr<Audio::SessionClientOfAudioServer>> create_client_from_handle(IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    return adopt_ref(*new Audio::SessionClientOfAudioServer(move(transport)));
}

static NonnullRefPtr<Media::IncrementallyPopulatedStream> load_test_file(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
}

static NonnullRefPtr<Media::Demuxer> create_demuxer(NonnullRefPtr<Media::IncrementallyPopulatedStream> const& stream)
{
    auto matroska_result = Media::Matroska::MatroskaDemuxer::from_stream(stream);
    if (!matroska_result.is_error())
        return matroska_result.release_value();
    return MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));
}

static ErrorOr<void> init_audio_server_client(ByteString const& server_name, BrokerBootstrap& bootstrap)
{
    TRY(close_child_bootstrap_fd(bootstrap));

    auto broker = TRY(create_broker_client(server_name, bootstrap.broker_fd));

    auto response = TRY(broker->connect_new_client("*"sv, "*"sv, true));
    auto client = TRY(create_client_from_handle(response.handle));

    Audio::SessionClientOfAudioServer::set_default_client(client);
    return {};
}
