/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <Ladybird/Utilities.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>
#include <LibMain/Main.h>
#include <WebDriver/Client.h>

static Vector<ByteString> certificates;

static ErrorOr<pid_t> launch_process(StringView application, ReadonlySpan<char const*> arguments)
{
    auto paths = TRY(get_paths_for_helper_process(application));

    ErrorOr<pid_t> result = -1;
    for (auto const& path : paths) {
        auto path_view = path.view();
        result = Core::Process::spawn(path_view, arguments, {}, Core::Process::KeepAsChild::Yes);
        if (!result.is_error())
            break;
    }
    return result;
}

static ErrorOr<pid_t> launch_browser(ByteString const& socket_path, bool use_qt_networking)
{
    auto arguments = Vector {
        "--webdriver-content-path",
        socket_path.characters(),
    };

    Vector<ByteString> certificate_args;
    for (auto const& certificate : certificates) {
        certificate_args.append(ByteString::formatted("--certificate={}", certificate));
        arguments.append(certificate_args.last().view().characters_without_null_termination());
    }

    arguments.append("--allow-popups");
    arguments.append("--force-new-process");
    if (use_qt_networking)
        arguments.append("--enable-qt-networking");

    arguments.append("about:blank");

    return launch_process("Ladybird"sv, arguments.span());
}

static ErrorOr<pid_t> launch_headless_browser(ByteString const& socket_path)
{
    auto resources = ByteString::formatted("{}/res", s_serenity_resource_root);
    return launch_process("headless-browser"sv,
        Array {
            "--resources",
            resources.characters(),
            "--webdriver-ipc-path",
            socket_path.characters(),
            "about:blank",
        });
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto listen_address = "0.0.0.0"sv;
    int port = 8000;
    bool enable_qt_networking = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(listen_address, "IP address to listen on", "listen-address", 'l', "listen_address");
    args_parser.add_option(port, "Port to listen on", "port", 'p', "port");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(enable_qt_networking, "Launch browser with Qt networking enabled", "enable-qt-networking");
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

    platform_init();

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

        auto launch_browser_callback = [&](ByteString const& socket_path) {
            return launch_browser(socket_path, enable_qt_networking);
        };

        auto maybe_client = WebDriver::Client::try_create(maybe_buffered_socket.release_value(), { move(launch_browser_callback), launch_headless_browser }, server);
        if (maybe_client.is_error()) {
            warnln("Could not create a WebDriver client: {}", maybe_client.error());
            return;
        }
    };

    TRY(server->listen(ipv4_address.value(), port, Core::TCPServer::AllowAddressReuse::Yes));
    outln("Listening on {}:{}", ipv4_address.value(), port);

    return loop.exec();
}
