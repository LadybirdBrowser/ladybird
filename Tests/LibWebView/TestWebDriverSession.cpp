/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/IPv4Address.h>
#include <AK/JsonObject.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <WebDriver/Client.h>
#include <WebDriver/Session.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#endif

struct SessionTest {
    ~SessionTest()
    {
        WebDriver::Session::close_all();
        if (browser_pid > 0) {
            (void)kill(browser_pid, SIGKILL);
            (void)waitpid(browser_pid, nullptr, 0);
        }
        browser_transport = nullptr;
        event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    ErrorOr<NonnullRefPtr<WebDriver::Session::NewSessionPromise>> start(bool connect_browser = false)
    {
#if !defined(AK_OS_MACOS)
        auto socket_directory = ByteString::formatted("{}/webdriver", TRY(Core::StandardPaths::runtime_directory()));
        TRY(Core::Directory::create(socket_directory, Core::Directory::CreateDirectories::Yes));
#endif
        auto server = TRY(Core::TCPServer::try_create());
        TRY(server->listen(IPv4Address(127, 0, 0, 1), 0));
        TRY(server->set_blocking(true));
        peer = TRY(Core::TCPSocket::connect("127.0.0.1", server->local_port().value()));
        auto socket = TRY(server->accept());
        auto buffered_socket = TRY(Core::BufferedTCPSocket::create(move(socket)));
        auto client = TRY(WebDriver::Client::try_create(move(buffered_socket), [this, connect_browser](auto const& endpoint, bool, auto const&) -> ErrorOr<Core::Process> {
            Array arguments { "60"sv };
            auto process = TRY(Core::Process::spawn("/bin/sleep"sv, arguments));
            browser_pid = process.pid();
            if (connect_browser) {
#if defined(AK_OS_MACOS)
                auto ports = TRY(IPC::bootstrap_transport_from_mach_server(endpoint));
                browser_transport = make<IPC::Transport>(move(ports.receive_right), move(ports.send_right));
#else
                browser_transport = TRY(IPC::Transport::from_socket(TRY(Core::LocalSocket::connect(endpoint))));
#endif
            }
            return process;
        }));
        weak_client = client->make_weak_ptr<WebDriver::Client>();
        return WebDriver::Session::create(client, JsonObject {}, Web::WebDriver::SessionFlags::Http);
    }

    bool wait_until(Function<bool()> condition)
    {
        bool timed_out = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { timed_out = true; });
        timer->start();
        event_loop.spin_until([&] { return condition() || timed_out; });
        timer->stop();
        return condition();
    }

    bool browser_exited()
    {
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(5);
        while (MonotonicTime::now() < deadline) {
            int status = 0;
            if (waitpid(browser_pid, &status, WNOHANG) == browser_pid) {
                browser_pid = -1;
                return WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
            }
            usleep(1000);
        }
        return false;
    }

    Core::EventLoop event_loop;
    OwnPtr<Core::TCPSocket> peer;
    OwnPtr<IPC::Transport> browser_transport;
    WeakPtr<WebDriver::Client> weak_client;
    pid_t browser_pid { -1 };
};

TEST_CASE(shutdown_closes_a_session_whose_browser_has_not_connected)
{
    SessionTest test;
    RefPtr promise = TRY_OR_FAIL(test.start());
    bool rejected = false;
    promise->when_rejected([&](auto& error) {
        EXPECT_EQ(error.error, "session not created"sv);
        rejected = true;
    });
    promise = nullptr;

    EXPECT(WebDriver::Session::has_pending_http_session_creation());
    WebDriver::Session::close_all();
    EXPECT(rejected);
    EXPECT(!WebDriver::Session::has_pending_http_session_creation());
    EXPECT(!test.weak_client);
    EXPECT(test.browser_exited());
}

TEST_CASE(browser_connection_loss_during_startup_releases_the_session)
{
    SessionTest test;
    RefPtr promise = TRY_OR_FAIL(test.start(true));
    bool rejected = false;
    promise->when_rejected([&](auto& error) {
        EXPECT_EQ(error.error, "session not created"sv);
        EXPECT(error.message.contains("Browser connection lost"sv));
        rejected = true;
    });
    promise = nullptr;
    test.browser_transport = nullptr;

    EXPECT(test.wait_until([&] { return rejected; }));
    EXPECT(!WebDriver::Session::has_pending_http_session_creation());
    EXPECT(!test.weak_client);
}
