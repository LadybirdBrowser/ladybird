/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@laybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Platform.h>
#include <AK/Random.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWebView/Utilities.h>
#include <WebDriver/Client.h>
#include <WebDriver/Session.h>

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

static Vector<ByteString> create_arguments(ByteString const& webdriver_endpoint, ByteString const& profile_path, bool headless, bool expose_experimental_interfaces, bool expose_internals_object, bool force_cpu_painting, bool disable_sandbox, Optional<StringView> debug_process, Optional<StringView> default_time_zone, Optional<StringView> resource_substitution_map_path)
{
    Vector<ByteString> arguments;
#if defined(AK_OS_MACOS)
    arguments.append("--webdriver-mach-server-name"sv);
#else
    arguments.append("--webdriver-content-path"sv);
#endif
    arguments.append(webdriver_endpoint);

    Vector<ByteString> certificate_args;
    for (auto const& certificate : certificates) {
        certificate_args.append(ByteString::formatted("--certificate={}", certificate));
        arguments.append(certificate_args.last().view().characters_without_null_termination());
    }

    if (headless)
        arguments.append("--headless"sv);

    arguments.append("--allow-popups"sv);
    arguments.append(ByteString::formatted("--profile-path={}", profile_path));
    arguments.append("--enable-autoplay"sv);
    arguments.append("--disable-scrollbar-painting"sv);
    if (expose_experimental_interfaces)
        arguments.append("--expose-experimental-interfaces"sv);
    if (expose_internals_object)
        arguments.append("--expose-internals-object"sv);
    if (force_cpu_painting)
        arguments.append("--force-cpu-painting"sv);
    if (disable_sandbox)
        arguments.append("--disable-sandbox"sv);

    if (debug_process.has_value())
        arguments.append(ByteString::formatted("--debug-process={}", debug_process.value()));

    if (default_time_zone.has_value())
        arguments.append(ByteString::formatted("--default-time-zone={}", default_time_zone.value()));

    if (resource_substitution_map_path.has_value())
        arguments.append(ByteString::formatted("--resource-map={}", resource_substitution_map_path.value()));

    // FIXME: WebDriver does not yet handle the WebContent process switch brought by site isolation.
    if (!Core::Environment::has("LADYBIRD_WEBDRIVER_ENABLE_SITE_ISOLATION"sv))
        arguments.append("--site-isolation=disable"sv);

    arguments.append("about:blank"sv);
    return arguments;
}

static void handle_signal(int signal)
{
    VERIFY(signal == SIGINT || signal == SIGTERM);
    Core::EventLoop::current().quit(0);
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto listen_address = "0.0.0.0"sv;
    int port = 8000;
    bool expose_experimental_interfaces = false;
    bool expose_internals_object = false;
    bool force_cpu_painting = false;
    bool disable_sandbox = false;
    bool headless = false;
    Optional<StringView> debug_process;
    Optional<StringView> default_time_zone;
    Optional<StringView> profiles_directory;
    Optional<StringView> resource_substitution_map_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(listen_address, "IP address to listen on", "listen-address", 'l', "listen_address");
    args_parser.add_option(port, "Port to listen on", "port", 'p', "port");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(expose_experimental_interfaces, "Expose experimental IDL interfaces", "expose-experimental-interfaces");
    args_parser.add_option(expose_internals_object, "Expose internals object", "expose-internals-object");
    args_parser.add_option(force_cpu_painting, "Launch browser with GPU painting disabled", "force-cpu-painting");
    args_parser.add_option(disable_sandbox, "Launch browser with helper process sandboxing disabled", "disable-sandbox");
    args_parser.add_option(debug_process, "Wait for a debugger to attach to the given process name (WebContent, RequestServer, etc.)", "debug-process", 0, "process-name");
    args_parser.add_option(headless, "Launch browser without a graphical interface", "headless");
    args_parser.add_option(default_time_zone, "Default time zone", "default-time-zone", 0, "time-zone-id");
    args_parser.add_option(profiles_directory, "Directory in which to create the browser profile", "profiles-directory", 0, "path");
    args_parser.add_option(resource_substitution_map_path, "Path to JSON file mapping URLs to local files", "resource-map", 0, "path");
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

    // Every browser instance launched by this WebDriver process shares one profile, which is removed on clean exit.
    auto profile_parent_directory = profiles_directory.has_value() ? ByteString { *profiles_directory } : Core::StandardPaths::tempfile_directory();
    auto profile_path = LexicalPath::join(profile_parent_directory, ByteString::formatted("ladybird-webdriver-profile-{:016x}-{:016x}", get_random<u64>(), get_random<u64>())).string();

    auto& loop = Core::EventLoop::initialize_for_current_thread();
    Core::EventLoop::register_signal(SIGINT, handle_signal);
    Core::EventLoop::register_signal(SIGTERM, handle_signal);
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

        auto launch_browser_callback = [&](ByteString const& webdriver_endpoint, bool headless) {
            auto arguments = create_arguments(webdriver_endpoint, profile_path, headless, expose_experimental_interfaces, expose_internals_object, force_cpu_painting, disable_sandbox, debug_process, default_time_zone, resource_substitution_map_path);
            return launch_process("Ladybird"sv, arguments.span());
        };

        auto maybe_client = WebDriver::Client::try_create(maybe_buffered_socket.release_value(), move(launch_browser_callback));
        if (maybe_client.is_error()) {
            warnln("Could not create a WebDriver client: {}", maybe_client.error());
            return;
        }

        auto client = maybe_client.release_value();
        // Capture a raw pointer here; a NonnullRefPtr would form a reference cycle through on_death,
        // keeping the client (and its socket) alive forever after the connection is closed.
        client->on_death = [&clients, client = client.ptr()] {
            clients.remove_all_matching([client](auto& other) { return other == client; });
        };
        clients.set(client);
    };

    TRY(server->listen(ipv4_address.value(), port, Core::TCPServer::AllowAddressReuse::Yes));
    outln("Listening on {}:{}", ipv4_address.value(), port);

    auto result = loop.exec();
    WebDriver::Session::close_all();

    if (FileSystem::exists(profile_path)) {
        if (auto removal_result = FileSystem::remove(profile_path, FileSystem::RecursionMode::Allowed); removal_result.is_error())
            warnln("Unable to remove WebDriver profile '{}': {}", profile_path, removal_result.error());
    }

    return result;
}
