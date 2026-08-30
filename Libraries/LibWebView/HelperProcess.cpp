/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/ScopeGuard.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibWebView/Application.h>
#include <LibWebView/CompositorClient.h>
#include <LibWebView/FontService.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Utilities.h>

#if defined(AK_OS_MACOS)
#    include <notify.h>
#    include <signal.h>
#    include <sys/wait.h>
#elif defined(AK_OS_LINUX)
#    include <signal.h>
#    include <sys/socket.h>
#    include <sys/wait.h>
#endif

namespace WebView {

static ErrorOr<Core::Process> launch_cpu_profiler(StringView server_name, pid_t pid, Optional<ByteString> const& configured_output)
{
#if defined(AK_OS_MACOS)
    auto output = configured_output.value_or(ByteString::formatted("Ladybird-{}-{}.trace", server_name, pid));
    auto notification_name = ByteString::formatted("org.ladybird.cpu-profiler-ready.{}.{}", Core::System::getpid(), pid);

    int notification_token = 0;
    if (notify_register_check(notification_name.characters(), &notification_token) != NOTIFY_STATUS_OK)
        return Error::from_string_literal("Unable to register for the xctrace startup notification");
    ScopeGuard cancel_notification = [&] { notify_cancel(notification_token); };

    Vector<ByteString> arguments {
        "record"sv,
        "--template"sv,
        "Time Profiler"sv,
        "--attach"sv,
        ByteString::number(pid),
        "--output"sv,
        output,
        "--notify-tracing-started"sv,
        notification_name,
        "--no-prompt"sv,
    };
    auto profiler = TRY(Core::Process::spawn({
        .executable = "xctrace"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    }));
    ArmedScopeGuard stop_profiler = [&] {
        (void)Core::System::kill(profiler.pid(), SIGINT);
        (void)profiler.wait_for_termination();
    };

    auto timer = Core::ElapsedTimer::start_new();
    for (;;) {
        int tracing_started = 0;
        if (notify_check(notification_token, &tracing_started) != NOTIFY_STATUS_OK)
            return Error::from_string_literal("Unable to check the xctrace startup notification");
        if (tracing_started)
            break;

        auto wait_result = TRY(Core::System::waitpid(profiler.pid(), WNOHANG));
        if (wait_result.pid != 0)
            return Error::from_string_literal("xctrace exited before profiling started");
        if (timer.elapsed_milliseconds() >= 30'000)
            return Error::from_string_literal("Timed out waiting for xctrace to start profiling");
        TRY(Core::System::sleep_ms(10));
    }

    dbgln("Launched {} process under Time Profiler; writing {}", server_name, output);
    stop_profiler.disarm();
    return profiler;
#elif defined(AK_OS_LINUX)
    auto output = configured_output.value_or(ByteString::formatted("perf.data.{}", pid));

    Array<int, 2> control_socket;
    Array<int, 2> acknowledgement_socket;
    TRY(Core::System::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control_socket.data()));
    TRY(Core::System::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, acknowledgement_socket.data()));
    TRY(Core::System::set_close_on_exec(control_socket[1], false));
    TRY(Core::System::set_close_on_exec(acknowledgement_socket[1], false));
    ScopeGuard close_sockets = [&] {
        for (auto fd : control_socket)
            (void)Core::System::close(fd);
        for (auto fd : acknowledgement_socket)
            (void)Core::System::close(fd);
    };

    Vector<ByteString> arguments {
        "record"sv,
        "--pid"sv,
        ByteString::number(pid),
        "--output"sv,
        output,
        "--delay=-1"sv,
        ByteString::formatted("--control=fd:{},{}", control_socket[1], acknowledgement_socket[1]),
    };
    auto profiler = TRY(Core::Process::spawn({
        .executable = "perf"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    }));
    TRY(Core::System::close(control_socket[1]));
    control_socket[1] = -1;
    TRY(Core::System::close(acknowledgement_socket[1]));
    acknowledgement_socket[1] = -1;
    ArmedScopeGuard stop_profiler = [&] {
        (void)Core::System::kill(profiler.pid(), SIGINT);
        (void)profiler.wait_for_termination();
    };

    // perf acknowledges this command only after it has attached and configured its events.
    static constexpr Array<u8, 7> enable_command { 'e', 'n', 'a', 'b', 'l', 'e', '\n' };
    auto sent = TRY(Core::System::send(control_socket[0], enable_command.span(), MSG_NOSIGNAL));
    if (sent != enable_command.size())
        return Error::from_string_literal("Unable to enable perf profiling");

    Array<u8, 4> acknowledgement;
    size_t received = 0;
    while (received < acknowledgement.size()) {
        auto bytes_read = TRY(Core::System::read(acknowledgement_socket[0], acknowledgement.span().slice(received)));
        if (bytes_read == 0)
            return Error::from_string_literal("perf exited before profiling started");
        received += bytes_read;
    }
    if (acknowledgement != Array<u8, 4> { 'a', 'c', 'k', '\n' })
        return Error::from_string_literal("perf returned an invalid startup acknowledgement");

    dbgln("Launched {} process under perf; writing {}", server_name, output);
    stop_profiler.disarm();
    return profiler;
#else
    (void)server_name;
    (void)pid;
    (void)configured_output;
    return Error::from_string_literal("CPU profiling is only supported on macOS and Linux");
#endif
}

template<typename ClientType, typename... ClientArguments>
static ErrorOr<NonnullRefPtr<ClientType>> launch_server_process(
    StringView server_name,
    Vector<ByteString> arguments,
    ClientArguments&&... client_arguments)
{
    auto process_type = WebView::process_type_from_name(server_name);
    auto const& browser_options = WebView::Application::browser_options();

    auto candidate_server_paths = TRY(get_paths_for_helper_process(server_name));

    if (browser_options.profile_helper_process == process_type && browser_options.profile_tool == ProfileTool::Callgrind) {
        arguments.prepend({
            "--tool=callgrind"sv,
            "--instr-atstart=no"sv,
            ""sv, // Placeholder for the process path.
        });
    }

    if (browser_options.debug_helper_processes.contains_slow(process_type))
        arguments.append("--wait-for-debugger"sv);

    for (auto [i, path] : enumerate(candidate_server_paths)) {
        Core::ProcessSpawnOptions options { .name = server_name, .arguments = arguments };

        if (browser_options.profile_helper_process == process_type && browser_options.profile_tool == ProfileTool::Callgrind) {
            options.executable = "valgrind"sv;
            options.search_for_executable_in_path = true;
            arguments[2] = path;
        } else {
            options.executable = path;
        }

        bool capture_output = WebView::Application::the().should_capture_web_content_output();
        auto result = WebView::Process::spawn<ClientType>(process_type, move(options), capture_output, forward<ClientArguments>(client_arguments)...);

        if (!result.is_error()) {
            auto&& [process, client] = result.release_value();

            if (WebView::Application::the().claim_cpu_profiler(process_type)) {
                auto profiler = TRY(launch_cpu_profiler(server_name, process.pid(), browser_options.profile_output));
                WebView::Application::the().set_cpu_profiler_process(move(profiler));
            }

            if constexpr (requires { client->set_pid(pid_t {}); })
                client->set_pid(process.pid());

            if constexpr (requires { client->transport().set_peer_pid(0); }) {
                auto response = client->template send_sync<typename ClientType::InitTransport>(Core::System::getpid());
                client->transport().set_peer_pid(response->peer_pid());
            }

            WebView::Application::the().add_child_process(move(process));

            if (browser_options.profile_helper_process == process_type && browser_options.profile_tool == ProfileTool::Callgrind) {
                dbgln();
                dbgln("\033[1;34mLaunched {} process under callgrind!\033[0m", server_name);
                dbgln("\033[1;36mRun `\033[4mcallgrind_control -i on\033[24m` to start instrumentation and `\033[4mcallgrind_control -i off\033[24m` stop it again.\033[0m");
                dbgln();
            }

            return move(client);
        }

        if (i == candidate_server_paths.size() - 1) {
            warnln("Could not launch any of {}: {}", candidate_server_paths, result.error());
            return result.release_error();
        }
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(IsPrivate is_private, u64 initial_page_id, Web::HTML::CrossProcessId root_navigable_id)
{
    auto const& browser_options = WebView::Application::browser_options();
    auto const& web_content_options = WebView::Application::web_content_options();

    Vector<ByteString> arguments;

    if (browser_options.headless_mode.has_value())
        arguments.append("--headless"sv);

    if (web_content_options.config_path.has_value()) {
        arguments.append("--config-path"sv);
        arguments.append(web_content_options.config_path.value());
    }
    if (web_content_options.cache_path.has_value()) {
        arguments.append("--cache-path"sv);
        arguments.append(web_content_options.cache_path.value());
    }
    if (web_content_options.is_test_mode == WebView::IsTestMode::Yes)
        arguments.append("--test-mode"sv);
    if (web_content_options.log_all_js_exceptions == WebView::LogAllJSExceptions::Yes)
        arguments.append("--log-all-js-exceptions"sv);
    arguments.append(ByteString::formatted("--site-isolation={}", WebView::site_isolation_mode_to_string(web_content_options.site_isolation_mode)));
    if (web_content_options.enable_http_memory_cache == WebView::EnableMemoryHTTPCache::Yes)
        arguments.append("--enable-http-memory-cache"sv);
    if (web_content_options.expose_experimental_interfaces == WebView::ExposeExperimentalInterfaces::Yes)
        arguments.append("--expose-experimental-interfaces"sv);
    if (web_content_options.expose_internals_object == WebView::ExposeInternalsObject::Yes)
        arguments.append("--expose-internals-object"sv);
    if (web_content_options.force_fontconfig == WebView::ForceFontconfig::Yes)
        arguments.append("--force-fontconfig"sv);
    if (web_content_options.collect_garbage_on_every_allocation == WebView::CollectGarbageOnEveryAllocation::Yes)
        arguments.append("--collect-garbage-on-every-allocation"sv);
    if (web_content_options.paint_viewport_scrollbars == PaintViewportScrollbars::No)
        arguments.append("--disable-scrollbar-painting"sv);
    if (web_content_options.enable_async_scrolling == EnableAsyncScrolling::No)
        arguments.append("--disable-async-scrolling"sv);
    if (web_content_options.file_scheme_urls_have_tuple_origins == FileSchemeUrlsHaveTupleOrigins::Yes)
        arguments.append("--tuple-file-origins"sv);
    if (browser_options.disable_sandbox == DisableSandbox::Yes)
        arguments.append("--disable-sandbox"sv);

    if (auto const maybe_echo_server_port = web_content_options.echo_server_port; maybe_echo_server_port.has_value()) {
        arguments.append("--echo-server-port"sv);
        arguments.append(ByteString::number(maybe_echo_server_port.value()));
    }

    if (web_content_options.default_time_zone.has_value()) {
        arguments.append("--default-time-zone");
        arguments.append(web_content_options.default_time_zone.value());
    }
    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    auto client = TRY(launch_server_process<WebView::WebContentClient>("WebContent"sv, move(arguments), is_private, initial_page_id, root_navigable_id));
    auto font_catalog = TRY(WebView::Application::font_service().clone_catalog());
    client->async_set_font_catalog(move(font_catalog.file), font_catalog.size, font_catalog.generation);
    if (auto system_font_family = WebView::Application::the().system_font_family(); system_font_family.has_value())
        client->async_set_system_font_family(system_font_family.release_value());
    return client;
}

ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process()
{
    auto const& browser_options = WebView::Application::browser_options();

    Vector<ByteString> arguments;
    if (browser_options.disable_sandbox == DisableSandbox::Yes)
        arguments.append("--disable-sandbox"sv);
    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    return launch_server_process<ImageDecoderClient::Client>("ImageDecoder"sv, arguments);
}

#if defined(HAVE_WASM_COMPILER_SERVICE)
ErrorOr<NonnullRefPtr<WasmCompilerClient::Client>> launch_wasm_compiler_process()
{
    auto const& browser_options = WebView::Application::browser_options();

    Vector<ByteString> arguments;
    if (browser_options.disable_sandbox == DisableSandbox::Yes)
        arguments.append("--disable-sandbox"sv);
    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    return launch_server_process<WasmCompilerClient::Client>("WasmCompiler"sv, arguments);
}
#endif

ErrorOr<NonnullRefPtr<WebView::CompositorClient>> launch_compositor_process()
{
    auto const& browser_options = WebView::Application::browser_options();
    auto const& web_content_options = WebView::Application::web_content_options();

    Vector<ByteString> arguments;

    if (web_content_options.cache_path.has_value()) {
        arguments.append("--cache-path"sv);
        arguments.append(web_content_options.cache_path.value());
    }
    if (browser_options.disable_sandbox == DisableSandbox::Yes)
        arguments.append("--disable-sandbox"sv);
    if (web_content_options.is_test_mode == WebView::IsTestMode::Yes)
        arguments.append("--test-mode"sv);
    if (web_content_options.force_cpu_painting == WebView::ForceCPUPainting::Yes)
        arguments.append("--force-cpu-painting"sv);
    if (web_content_options.force_fontconfig == WebView::ForceFontconfig::Yes)
        arguments.append("--force-fontconfig"sv);
    if (web_content_options.enable_async_scrolling == EnableAsyncScrolling::No)
        arguments.append("--disable-async-scrolling"sv);
    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    auto client = TRY(launch_server_process<WebView::CompositorClient>("Compositor"sv, move(arguments)));
    auto font_catalog = TRY(WebView::Application::font_service().clone_catalog());
    client->async_set_font_catalog(move(font_catalog.file), font_catalog.size, font_catalog.generation);
    return client;
}

ErrorOr<NonnullRefPtr<WebWorkerClient>> launch_web_worker_process(Web::HTML::AgentType type, IsPrivate is_private, Web::HTML::WorkerAgentId agent_id)
{
    auto const& browser_options = WebView::Application::browser_options();
    auto const& web_content_options = WebView::Application::web_content_options();

    Vector<ByteString> arguments;

    if (web_content_options.cache_path.has_value()) {
        arguments.append("--cache-path"sv);
        arguments.append(web_content_options.cache_path.value());
    }

    if (browser_options.disable_sandbox == DisableSandbox::Yes)
        arguments.append("--disable-sandbox"sv);
    if (web_content_options.expose_experimental_interfaces == WebView::ExposeExperimentalInterfaces::Yes)
        arguments.append("--expose-experimental-interfaces"sv);
    if (web_content_options.enable_http_memory_cache == WebView::EnableMemoryHTTPCache::Yes)
        arguments.append("--enable-http-memory-cache"sv);
    if (web_content_options.file_scheme_urls_have_tuple_origins == FileSchemeUrlsHaveTupleOrigins::Yes)
        arguments.append("--tuple-file-origins"sv);

    arguments.append("--type"sv);
    switch (type) {
    case Web::HTML::AgentType::DedicatedWorker:
        arguments.append("dedicated"sv);
        break;
    case Web::HTML::AgentType::SharedWorker:
        arguments.append("shared"sv);
        break;
    case Web::HTML::AgentType::ServiceWorker:
        arguments.append("service"sv);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    auto client = TRY(launch_server_process<WebWorkerClient>("WebWorker"sv, move(arguments), is_private, agent_id));
    auto font_catalog = TRY(WebView::Application::font_service().clone_catalog());
    client->async_set_font_catalog(move(font_catalog.file), font_catalog.size, font_catalog.generation);
    if (auto system_font_family = WebView::Application::the().system_font_family(); system_font_family.has_value())
        client->async_set_system_font_family(system_font_family.release_value());
    return client;
}

ErrorOr<NonnullRefPtr<Requests::RequestClient>> launch_request_server_process()
{
    auto const& browser_options = Application::browser_options();
    auto const& request_server_options = Application::request_server_options();

    Vector<ByteString> arguments;

    arguments.append("--cache-path"sv);
    arguments.append(request_server_options.cache_path);

    if (browser_options.disable_sandbox == DisableSandbox::Yes)
        arguments.append("--disable-sandbox"sv);
    for (auto const& certificate : request_server_options.certificates)
        arguments.append(ByteString::formatted("--certificate={}", certificate));

    arguments.append("--http-disk-cache-mode"sv);

    switch (request_server_options.http_disk_cache_mode) {
    case HTTPDiskCacheMode::Disabled:
        arguments.append("disabled"sv);
        break;
    case HTTPDiskCacheMode::Enabled:
        arguments.append("enabled"sv);
        break;
    case HTTPDiskCacheMode::Testing:
        arguments.append("testing"sv);
        break;
    }

    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    if (request_server_options.resource_substitution_map_path.has_value())
        arguments.append(ByteString::formatted("--resource-map={}", *request_server_options.resource_substitution_map_path));

    auto client = TRY(launch_server_process<Requests::RequestClient>("RequestServer"sv, move(arguments)));

    auto const& browsing_data_settings = Application::settings().browsing_data_settings();
    client->async_set_disk_cache_settings(browsing_data_settings.disk_cache_settings);

    Application::settings().dns_settings().visit(
        [](SystemDNS) {},
        [&](DNSOverTLS const& dns_over_tls) {
            dbgln("Setting DNS server to {}:{} with TLS ({} local dnssec)", dns_over_tls.server_address, dns_over_tls.port, dns_over_tls.validate_dnssec_locally ? "with" : "without");
            client->async_set_dns_server(dns_over_tls.server_address, dns_over_tls.port, true, dns_over_tls.validate_dnssec_locally);
        },
        [&](DNSOverUDP const& dns_over_udp) {
            dbgln("Setting DNS server to {}:{} ({} local dnssec)", dns_over_udp.server_address, dns_over_udp.port, dns_over_udp.validate_dnssec_locally ? "with" : "without");
            client->async_set_dns_server(dns_over_udp.server_address, dns_over_udp.port, false, dns_over_udp.validate_dnssec_locally);
        });

    return client;
}

ErrorOr<IPC::TransportHandle> connect_new_request_server_client(IsPrivate is_private)
{
    auto response = Application::request_server_client().send_sync_but_allow_failure<Messages::RequestServer::ConnectNewClient>(is_private == IsPrivate::Yes ? RequestServer::IsPrivate::Yes : RequestServer::IsPrivate::No);
    if (!response)
        return Error::from_string_literal("Failed to connect to RequestServer");
    return response->take_handle();
}

ErrorOr<IPC::TransportHandle> connect_new_image_decoder_client()
{
    auto response = Application::image_decoder_client().send_sync_but_allow_failure<Messages::ImageDecoderServer::ConnectNewClients>(1);
    if (!response)
        return Error::from_string_literal("Failed to connect to ImageDecoder");

    auto handles = response->take_handles();
    if (handles.size() != 1)
        return Error::from_string_literal("Failed to connect to ImageDecoder");
    return handles.take_last();
}

#if defined(HAVE_WASM_COMPILER_SERVICE)
ErrorOr<IPC::TransportHandle> connect_new_wasm_compiler_client()
{
    auto response = Application::wasm_compiler_client().send_sync_but_allow_failure<Messages::WasmCompilerServer::ConnectNewClients>(1);
    if (!response)
        return Error::from_string_literal("Failed to connect to WasmCompiler");

    auto handles = response->take_handles();
    if (handles.size() != 1)
        return Error::from_string_literal("Failed to connect to WasmCompiler");
    return handles.take_last();
}
#endif

}
