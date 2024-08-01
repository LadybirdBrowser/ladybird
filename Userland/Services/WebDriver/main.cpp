/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>
#include <LibMain/Main.h>
#include <WebDriver/Client.h>

static ErrorOr<pid_t> launch_browser(ByteString const& socket_path)
{
    return Core::Process::spawn("/bin/Browser"sv,
        Array {
            "--webdriver-content-path",
            socket_path.characters(),
            "about:blank",
        });
}

static ErrorOr<pid_t> launch_headless_browser(ByteString const& socket_path)
{
    return Core::Process::spawn("/bin/headless-browser"sv,
        Array {
            "--webdriver-content-path",
            socket_path.characters(),
            "about:blank",
        });
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    ByteString default_listen_address = "0.0.0.0";
    u16 default_port = 8000;

    ByteString listen_address = default_listen_address;
    int port = default_port;

    Core::ArgsParser args_parser;
    args_parser.add_option(listen_address, "IP address to listen on", "listen-address", 'l', "listen_address");
    args_parser.add_option(port, "Port to listen on", "port", 'p', "port");
    args_parser.parse(arguments);

    auto ipv4_address = IPv4Address::from_string(listen_address);
    if (!ipv4_address.has_value()) {
        warnln("Invalid listen address: {}", listen_address);
        return 1;
    }

    if ((u16)port != port) {
        warnln("Invalid port number: {}", port);
        return 1;
    }

    TRY(Core::System::pledge("stdio accept cpath rpath recvfd inet unix proc exec fattr"));

    auto webdriver_socket_path = ByteString::formatted("{}/webdriver", TRY(Core::StandardPaths::runtime_directory()));
    TRY(Core::Directory::create(webdriver_socket_path, Core::Directory::CreateDirectories::Yes));

    Core::EventLoop loop;

    auto server = TRY(Core::TCPServer::try_create());

    // FIXME: Propagate errors
    server->on_ready_to_accept = [&] {
        auto maybe_client_socket = server->accept();
        if (maybe_client_socket.is_error()) {
            warnln("Failed to accept the client: {}", maybe_client_socket.error());
            return;
        }

        auto maybe_buffered_socket = Core::BufferedTCPSocket::create(maybe_client_socket.release_value());
        if (maybe_buffered_socket.is_error()) {
            warnln("Could not obtain a buffered socket for the client: {}", maybe_buffered_socket.error());
            return;
        }

        auto maybe_client = WebDriver::Client::try_create(maybe_buffered_socket.release_value(), { launch_browser, launch_headless_browser }, server);
        if (maybe_client.is_error()) {
            warnln("Could not create a WebDriver client: {}", maybe_client.error());
            return;
        }
    };

    TRY(server->listen(ipv4_address.value(), port));

    outln("Listening on {}:{}", ipv4_address.value(), port);

    TRY(Core::System::unveil("/bin/Browser", "rx"));
    TRY(Core::System::unveil("/bin/headless-browser", "rx"));
    TRY(Core::System::unveil("/etc/timezone", "r"));
    TRY(Core::System::unveil("/res/icons", "r"));
    TRY(Core::System::unveil(webdriver_socket_path, "rwc"sv));
    TRY(Core::System::unveil(nullptr, nullptr));

    TRY(Core::System::pledge("stdio accept cpath rpath recvfd unix proc exec fattr"));
    return loop.exec();
}
