/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibTest/TestCase.h>
#include <LibWebView/AudioDaemonSocket.h>

TEST_CASE(a_configured_unix_server_keeps_its_transport_prefix)
{
    auto path = WebView::unix_path_from_server_string("unix:/custom/pulse/native"sv);
    EXPECT(path.has_value());
    if (path.has_value())
        EXPECT_EQ(path.value(), ByteString { "/custom/pulse/native" });
}

TEST_CASE(a_bare_path_is_taken_as_it_is)
{
    auto path = WebView::unix_path_from_server_string("/run/user/1000/pulse/native"sv);
    EXPECT(path.has_value());
    if (path.has_value())
        EXPECT_EQ(path.value(), ByteString { "/run/user/1000/pulse/native" });
}

TEST_CASE(a_machine_identifier_in_front_of_the_address_is_ignored)
{
    auto path = WebView::unix_path_from_server_string("{0123456789abcdef}unix:/custom/sock"sv);
    EXPECT(path.has_value());
    if (path.has_value())
        EXPECT_EQ(path.value(), ByteString { "/custom/sock" });
}

TEST_CASE(a_server_that_is_not_a_unix_socket_is_not_a_path)
{
    // Neither of these can be reached by a process with no sockets of its own.
    EXPECT(!WebView::unix_path_from_server_string("tcp:198.51.100.7:4713"sv).has_value());
    EXPECT(!WebView::unix_path_from_server_string("audio.example"sv).has_value());
}

#if defined(HAVE_PULSEAUDIO)

namespace {

// Every variable the lookup consults, so a test states the whole environment rather than inheriting whatever the
// machine has set. CI runs with PULSE_SERVER pointing at its own server, which otherwise takes precedence over
// anything a test configures.
class ScopedAudioEnvironment {
public:
    static constexpr Array managed_variables { "PULSE_SERVER"sv, "PULSE_CLIENTCONFIG"sv, "PULSE_RUNTIME_PATH"sv };

    ScopedAudioEnvironment()
    {
        for (auto name : managed_variables) {
            if (auto value = Core::Environment::get(name); value.has_value())
                m_saved.append({ name, ByteString { value.value() } });
            MUST(Core::Environment::unset(name));
        }
    }

    ~ScopedAudioEnvironment()
    {
        // Clear first: a test may have set a variable that was absent to begin with, and restoring only what was
        // saved would leave that one behind for whatever runs next.
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

TEST_CASE(a_daemon_that_is_not_there_is_an_error)
{
    // The broker turns this into a refused connection, which is what the helper's library expects of a daemon that is
    // down. Nothing listens on this path, so the answer comes at once rather than at the deadline.
    ScopedAudioEnvironment environment;
    environment.set("PULSE_SERVER"sv, "unix:/tmp/ladybird-audio-not-running"sv);

    EXPECT(WebView::audio_daemon_socket_path().is_error());
}

#endif
