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
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Sinks/AudioMixingSink.h>
#include <LibMedia/TrackType.h>
#include <LibTest/TestSuite.h>

static ErrorOr<ByteString> find_audioserver_executable_path();
static ErrorOr<Core::Process> spawn_audioserver_with_takeover(int takeover_fd);
static ErrorOr<NonnullRefPtr<AudioServer::BrokerOfAudioServer>> create_broker_client_from_fd(int fd);
static ErrorOr<NonnullRefPtr<AudioServer::SessionClientOfAudioServer>> create_client_from_fd(int fd);
static ErrorOr<void> init_audio_server_client(int fds[2]);
static NonnullRefPtr<Media::IncrementallyPopulatedStream> load_test_file(StringView path);
static NonnullRefPtr<Media::Demuxer> create_demuxer(NonnullRefPtr<Media::IncrementallyPopulatedStream> const& stream);

TEST_CASE(create_and_destroy_audio_mixing_sink)
{
#if !defined(HAVE_PULSEAUDIO) && !defined(AK_OS_MACOS)
    return;
#endif
    Core::EventLoop event_loop;
    int fds[2] {};
    auto socketpair_result = Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds);
    if (socketpair_result.is_error()) {
        FAIL(ByteString::formatted("socketpair(AF_LOCAL, SOCK_STREAM) failed: {}", socketpair_result.error()));
        return;
    }

    auto process_result = spawn_audioserver_with_takeover(fds[1]);
    if (process_result.is_error()) {
        FAIL(ByteString::formatted("Failed to spawn AudioServer with SOCKET_TAKEOVER: {}", process_result.error()));
        return;
    }

    auto audio_server_process = process_result.release_value();
    auto cleanup_audio_server = AK::ArmedScopeGuard([&] {
        AudioServer::SessionClientOfAudioServer::set_default_client(nullptr);
        (void)Core::System::kill(audio_server_process.pid(), SIGTERM);
        (void)audio_server_process.wait_for_termination();
    });

    MUST(init_audio_server_client(fds));

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

        bool did_decode_audio = false;
        bool hit_error = false;
        provider->set_block_end_time_handler([&](AK::Duration) {
            did_decode_audio = true;
        });
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
        while (!did_decode_audio && !hit_error) {
            if (MonotonicTime::now_coarse() - start >= timeout)
                break;
            event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }

        bool current_time_advanced = sink->current_time() > AK::Duration::zero();
        while (!current_time_advanced && !hit_error) {
            if (MonotonicTime::now_coarse() - start >= timeout)
                break;
            event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            current_time_advanced = sink->current_time() > AK::Duration::zero();
        }

        if (!did_decode_audio)
            FAIL("Expected at least one decoded audio block before timeout");
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

    AudioServer::SessionClientOfAudioServer::set_default_client(nullptr);

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

static ErrorOr<Core::Process> spawn_audioserver_with_takeover(int takeover_fd)
{
    auto audio_server_path = TRY(find_audioserver_executable_path());
    Vector<ByteString> child_arguments;

    auto takeover_string = ByteString::formatted("test-audio-mixing-sink:{}", takeover_fd);
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));

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

static ErrorOr<NonnullRefPtr<AudioServer::BrokerOfAudioServer>> create_broker_client_from_fd(int fd)
{
    auto socket = TRY(Core::LocalSocket::adopt_fd(fd));
    auto transport = make<IPC::Transport>(move(socket));
    return adopt_ref(*new AudioServer::BrokerOfAudioServer(move(transport)));
}

static ErrorOr<NonnullRefPtr<AudioServer::SessionClientOfAudioServer>> create_client_from_fd(int fd)
{
    auto socket = TRY(Core::LocalSocket::adopt_fd(fd));
    auto transport = make<IPC::Transport>(move(socket));
    return adopt_ref(*new AudioServer::SessionClientOfAudioServer(move(transport)));
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

static ErrorOr<void> init_audio_server_client(int fds[2])
{
    TRY(Core::System::close(fds[1]));

    auto broker = TRY(create_broker_client_from_fd(fds[0]));

    auto response = TRY(broker->connect_new_client("*"sv, "*"sv, true));
    auto client = TRY(create_client_from_fd(response.socket.take_fd()));

    AudioServer::SessionClientOfAudioServer::set_default_client(client);
    return {};
}
