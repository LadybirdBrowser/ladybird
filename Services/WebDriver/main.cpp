/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@laybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>
#include <LibMain/Main.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWebView/Utilities.h>
#include <WebDriver/Client.h>

static Vector<ByteString> certificates;

static ErrorOr<Core::Process> launch_process(StringView application, ReadonlySpan<ByteString> arguments)
{
    auto paths = TRY(WebView::get_paths_for_helper_process(application));

    ErrorOr<Core::Process> result = Error::from_string_literal("All paths failed to launch");
    for (auto const& path : paths) {
        auto path_view = path.view();
        result = Core::Process::spawn(path_view, arguments);
        if (!result.is_error())
            break;
    }
    return result;
}

static Vector<ByteString> create_arguments(ByteString const& socket_path, bool headless, bool force_cpu_painting, Optional<StringView> debug_process, Optional<StringView> default_time_zone)
{
    Vector<ByteString> arguments {
        "--webdriver-content-path"sv,
        socket_path,
    };

    Vector<ByteString> certificate_args;
    for (auto const& certificate : certificates) {
        certificate_args.append(ByteString::formatted("--certificate={}", certificate));
        arguments.append(certificate_args.last().view().characters_without_null_termination());
    }

    if (headless)
        arguments.append("--headless"sv);

    arguments.append("--allow-popups"sv);
    arguments.append("--force-new-process"sv);
    arguments.append("--enable-autoplay"sv);
    arguments.append("--disable-scrollbar-painting"sv);
    if (force_cpu_painting)
        arguments.append("--force-cpu-painting"sv);

    if (debug_process.has_value())
        arguments.append(ByteString::formatted("--debug-process={}", debug_process.value()));

    if (default_time_zone.has_value())
        arguments.append(ByteString::formatted("--default-time-zone={}", default_time_zone.value()));

    // FIXME: WebDriver does not yet handle the WebContent process switch brought by site isolation.
    arguments.append("--disable-site-isolation"sv);

    arguments.append("about:blank"sv);
    return arguments;
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto listen_address = "0.0.0.0"sv;
    int port = 8000;
    bool force_cpu_painting = false;
    bool headless = false;
    Optional<StringView> debug_process;
    Optional<StringView> default_time_zone;

    Core::ArgsParser args_parser;
    args_parser.add_option(listen_address, "IP address to listen on", "listen-address", 'l', "listen_address");
    args_parser.add_option(port, "Port to listen on", "port", 'p', "port");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(force_cpu_painting, "Launch browser with GPU painting disabled", "force-cpu-painting");
    args_parser.add_option(debug_process, "Wait for a debugger to attach to the given process name (WebContent, RequestServer, etc.)", "debug-process", 0, "process-name");
    args_parser.add_option(headless, "Launch browser without a graphical interface", "headless");
    args_parser.add_option(default_time_zone, "Default time zone", "default-time-zone", 0, "time-zone-id");
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

    WebView::platform_init();

    Web::WebDriver::set_default_interface_mode(headless ? Web::WebDriver::InterfaceMode::Headless : Web::WebDriver::InterfaceMode::Graphical);

    auto webdriver_socket_path = ByteString::formatted("{}/webdriver", TRY(Core::StandardPaths::runtime_directory()));
    TRY(Core::Directory::create(webdriver_socket_path, Core::Directory::CreateDirectories::Yes));

    Core::EventLoop loop;
    auto server = TRY(Core::TCPServer::try_create());

    HashTable<NonnullRefPtr<WebDriver::Client>> clients;

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

        auto launch_browser_callback = [&](ByteString const& socket_path, bool headless) {
            auto arguments = create_arguments(socket_path, headless, force_cpu_painting, debug_process, default_time_zone);
            return launch_process("Ladybird"sv, arguments.span());
        };

        auto maybe_client = WebDriver::Client::try_create(maybe_buffered_socket.release_value(), move(launch_browser_callback));
        if (maybe_client.is_error()) {
            warnln("Could not create a WebDriver client: {}", maybe_client.error());
            return;
        }

        auto client = maybe_client.release_value();
        client->on_death = [&clients, client] {
            clients.remove(client);
        };
        clients.set(client);
    };

    TRY(server->listen(ipv4_address.value(), port, Core::TCPServer::AllowAddressReuse::Yes));
    outln("Listening on {}:{}", ipv4_address.value(), port);

    return loop.exec();
}
