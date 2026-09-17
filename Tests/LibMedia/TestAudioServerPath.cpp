/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/AudioServerPath.h>
#include <LibTest/TestCase.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Every variable the lookup consults, so a test states the whole environment rather than
// inheriting whatever the machine has set. CI runs with PULSE_SERVER pointing at its own server,
// which otherwise takes precedence over anything a test configures.
class ScopedAudioEnvironment {
public:
    static constexpr Array managed_variables { "PULSE_SERVER"sv, "PULSE_CLIENTCONFIG"sv, "PULSE_RUNTIME_PATH"sv };

    ScopedAudioEnvironment()
    {
        for (auto name : managed_variables) {
            if (auto value = Core::Environment::get(name); value.has_value())
                m_saved.append({ name, ByteString { *value } });
            MUST(Core::Environment::unset(name));
        }
    }

    ~ScopedAudioEnvironment()
    {
        // Clear first: a test may have set a variable that was absent to begin with, and restoring
        // only what was saved would leave that one behind for whatever runs next.
        for (auto name : managed_variables)
            MUST(Core::Environment::unset(name));
        for (auto const& [name, value] : m_saved)
            MUST(Core::Environment::set(name, value, Core::Environment::Overwrite::Yes));
    }

    void set(StringView name, StringView value) const
    {
        MUST(Core::Environment::set(name, value, Core::Environment::Overwrite::Yes));
    }

private:
    struct SavedVariable {
        StringView name;
        ByteString value;
    };
    Vector<SavedVariable> m_saved;
};

}

#if defined(HAVE_PULSEAUDIO)

// These candidates are what a sandbox allows ahead of time, so anything reported here is reachable
// from inside one and anything missing is a socket the sandbox will refuse. Without the audio
// library there is nothing to name, so these only mean anything when it is built in.

TEST_CASE(the_default_runtime_socket_is_always_offered)
{
    ScopedAudioEnvironment environment;

    // Whatever the audio library reports, the usual socket stays on the list, so a server that was
    // not running when we asked can still be reached once it starts.
    auto candidates = Audio::audio_server_path_candidates();
    EXPECT(!candidates.is_empty());

    auto runtime_directory = MUST(Core::StandardPaths::runtime_directory());
    EXPECT(candidates.contains_slow(ByteString::formatted("{}/pulse/native", runtime_directory)));
}

TEST_CASE(a_configured_runtime_directory_is_offered)
{
    ScopedAudioEnvironment environment;
    environment.set("PULSE_RUNTIME_PATH"sv, "/tmp/ladybird-audio-runtime"sv);

    auto candidates = Audio::audio_server_path_candidates();
    EXPECT(candidates.contains_slow(ByteString { "/tmp/ladybird-audio-runtime/native" }));
}

TEST_CASE(every_candidate_is_an_absolute_path)
{
    ScopedAudioEnvironment environment;

    // A relative path or a host name would mean allowing something that is not a socket we can
    // reason about, and a TCP server cannot be reached from a process with no sockets at all.
    for (auto const& candidate : Audio::audio_server_path_candidates())
        EXPECT(candidate.starts_with('/'));
}

TEST_CASE(candidates_are_not_repeated)
{
    ScopedAudioEnvironment environment;

    // The list goes straight into an allowlist, so duplicates would just be noise there.
    auto candidates = Audio::audio_server_path_candidates();
    for (size_t i = 0; i < candidates.size(); ++i) {
        for (size_t j = i + 1; j < candidates.size(); ++j)
            EXPECT_NE(candidates[i], candidates[j]);
    }
}

#endif

TEST_CASE(a_configured_unix_server_keeps_its_transport_prefix)
{
    auto path = Audio::unix_path_from_server_string("unix:/custom/pulse/native"sv);
    EXPECT(path.has_value());
    if (path.has_value())
        EXPECT_EQ(*path, ByteString { "/custom/pulse/native" });
}

TEST_CASE(a_bare_path_is_taken_as_it_is)
{
    auto path = Audio::unix_path_from_server_string("/run/user/1000/pulse/native"sv);
    EXPECT(path.has_value());
    if (path.has_value())
        EXPECT_EQ(*path, ByteString { "/run/user/1000/pulse/native" });
}

TEST_CASE(a_machine_identifier_in_front_of_the_address_is_ignored)
{
    auto path = Audio::unix_path_from_server_string("{0123456789abcdef}unix:/custom/sock"sv);
    EXPECT(path.has_value());
    if (path.has_value())
        EXPECT_EQ(*path, ByteString { "/custom/sock" });
}

TEST_CASE(a_server_that_is_not_a_unix_socket_is_not_a_path)
{
    // Neither of these can be reached by a process with no sockets of its own, so there is nothing
    // here for an allowlist to hold.
    EXPECT(!Audio::unix_path_from_server_string("tcp:198.51.100.7:4713"sv).has_value());
    EXPECT(!Audio::unix_path_from_server_string("audio.example"sv).has_value());
}

#if defined(HAVE_PULSEAUDIO)

TEST_CASE(a_configured_socket_is_offered_even_when_nothing_is_listening)
{
    // The allowlist a renderer is given cannot be changed afterwards, so a server that is merely
    // not running yet must still be reachable once it starts.
    ScopedAudioEnvironment environment;
    environment.set("PULSE_SERVER"sv, "unix:/tmp/ladybird-audio-not-running"sv);

    auto candidates = Audio::audio_server_path_candidates();
    EXPECT(candidates.contains_slow(ByteString { "/tmp/ladybird-audio-not-running" }));
}

TEST_CASE(every_configured_fallback_socket_is_offered)
{
    // The setting holds a list and the library works down it, so the one that answers may not be
    // the one written first.
    ScopedAudioEnvironment environment;
    environment.set("PULSE_SERVER"sv, "unix:/tmp/ladybird-audio-a unix:/tmp/ladybird-audio-b"sv);

    auto candidates = Audio::audio_server_path_candidates();
    EXPECT(candidates.contains_slow(ByteString { "/tmp/ladybird-audio-a" }));
    EXPECT(candidates.contains_slow(ByteString { "/tmp/ladybird-audio-b" }));
}

TEST_CASE(a_configured_tcp_server_is_still_not_offered)
{
    ScopedAudioEnvironment environment;
    environment.set("PULSE_SERVER"sv, "tcp:198.51.100.7:4713"sv);

    for (auto const& candidate : Audio::audio_server_path_candidates())
        EXPECT(!candidate.contains("198.51.100.7"sv));
}

TEST_CASE(a_socket_named_only_by_configuration_is_offered_when_its_server_is_down)
{
    // The case that a list built only from what answered would miss: the socket is named in a
    // configuration file rather than the environment, and nothing is listening on it yet. The list
    // a renderer is given is fixed when it starts, so missing it here means audio stays broken in
    // that renderer even after the server comes up.
    //
    // PULSE_SERVER has to be out of the way for this to mean anything, because it wins over the
    // configuration file, and a machine that already runs a server usually sets it.
    ScopedAudioEnvironment environment;

    char directory_template[] = "/tmp/ladybird-audio-config-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);

    auto configuration_path = ByteString::formatted("{}/client.conf", directory);
    {
        auto file = MUST(Core::File::open(configuration_path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted("default-server = unix:/tmp/ladybird-audio-configured\n"sv.bytes()));
    }

    environment.set("PULSE_CLIENTCONFIG"sv, configuration_path);

    auto candidates = Audio::audio_server_path_candidates();

    MUST(Core::System::unlink(configuration_path));
    MUST(Core::System::rmdir(StringView { directory, strlen(directory) }));

    EXPECT(candidates.contains_slow(ByteString { "/tmp/ladybird-audio-configured" }));
}

#endif
