/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CameraDaemonSocket.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace {

// Every variable the lookup consults, so a test states the whole environment rather than inheriting the machine's.
class ScopedCameraEnvironment {
public:
    static constexpr Array managed_variables { "PIPEWIRE_REMOTE"sv, "PIPEWIRE_RUNTIME_DIR"sv, "XDG_RUNTIME_DIR"sv };

    ScopedCameraEnvironment()
    {
        for (auto name : managed_variables) {
            if (auto value = Core::Environment::get(name); value.has_value())
                m_saved.append({ name, ByteString { value.value() } });
            MUST(Core::Environment::unset(name));
        }
    }

    ~ScopedCameraEnvironment()
    {
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

ByteString make_directory()
{
    char directory_template[] = "/tmp/camera-daemon-socket-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);
    return directory;
}

// A socket that is bound but never listened on is still a socket where one is looked for.
void make_socket(ByteString const& path)
{
    auto fd = MUST(Core::System::socket(AF_UNIX, SOCK_STREAM, 0));
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    VERIFY(path.length() < sizeof(address.sun_path));
    memcpy(address.sun_path, path.characters(), path.length());
    MUST(Core::System::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
    MUST(Core::System::close(fd));
}

}

TEST_CASE(a_name_is_looked_for_in_the_runtime_directory)
{
    ScopedCameraEnvironment environment;
    auto directory = make_directory();
    ScopeGuard remove_directory = [&] { (void)FileSystem::remove(directory, FileSystem::RecursionMode::Allowed); };
    environment.set("XDG_RUNTIME_DIR"sv, directory);
    make_socket(LexicalPath::join(directory, "pipewire-0"sv).string());

    EXPECT_EQ(MUST(WebView::camera_daemon_socket_path()), LexicalPath::join(directory, "pipewire-0"sv).string());
}

// NB: The system daemon is outside our control. Test only that missing sockets leave the user directory preferred.
TEST_CASE(with_no_socket_anywhere_the_user_directory_is_named)
{
    ScopedCameraEnvironment environment;
    auto directory = make_directory();
    ScopeGuard remove_directory = [&] { (void)FileSystem::remove(directory, FileSystem::RecursionMode::Allowed); };
    environment.set("XDG_RUNTIME_DIR"sv, directory);
    environment.set("PIPEWIRE_REMOTE"sv, "camera-daemon-socket-test-missing"sv);

    auto path = MUST(WebView::camera_daemon_socket_path());
    EXPECT_EQ(path, LexicalPath::join(directory, "camera-daemon-socket-test-missing"sv).string());
    auto system_path = ByteString { "/run/pipewire/camera-daemon-socket-test-missing" };
    EXPECT(path != system_path);
}

TEST_CASE(an_array_of_remotes_is_tried_in_turn)
{
    ScopedCameraEnvironment environment;
    auto directory = make_directory();
    ScopeGuard remove_directory = [&] { (void)FileSystem::remove(directory, FileSystem::RecursionMode::Allowed); };
    environment.set("PIPEWIRE_RUNTIME_DIR"sv, directory);
    make_socket(LexicalPath::join(directory, "second"sv).string());

    // PipeWire's JSON needs neither quotes nor commas.
    environment.set("PIPEWIRE_REMOTE"sv, "[ \"first\", second ]"sv);
    EXPECT_EQ(MUST(WebView::camera_daemon_socket_path()), LexicalPath::join(directory, "second"sv).string());
    environment.set("PIPEWIRE_REMOTE"sv, "[first second]"sv);
    EXPECT_EQ(MUST(WebView::camera_daemon_socket_path()), LexicalPath::join(directory, "second"sv).string());
}

TEST_CASE(a_remote_that_is_a_path_is_taken_as_it_is)
{
    ScopedCameraEnvironment environment;
    auto directory = make_directory();
    ScopeGuard remove_directory = [&] { (void)FileSystem::remove(directory, FileSystem::RecursionMode::Allowed); };
    auto socket_path = LexicalPath::join(directory, "elsewhere"sv).string();
    make_socket(socket_path);
    environment.set("PIPEWIRE_REMOTE"sv, ByteString::formatted("[ missing \"{}\" ]", socket_path));
    EXPECT_EQ(MUST(WebView::camera_daemon_socket_path()), socket_path);
}
