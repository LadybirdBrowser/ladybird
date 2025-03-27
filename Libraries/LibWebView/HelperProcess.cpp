/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibCore/Process.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Utilities.h>

namespace WebView {

template<typename ClientType, typename... ClientArguments>
static ErrorOr<NonnullRefPtr<ClientType>> launch_server_process(
    StringView server_name,
    Vector<ByteString> arguments,
    ClientArguments&&... client_arguments)
{
    auto process_type = WebView::process_type_from_name(server_name);
    auto const& browser_options = WebView::Application::browser_options();

    auto candidate_server_paths = TRY(get_paths_for_helper_process(server_name));

    if (browser_options.profile_helper_process == process_type) {
        arguments.prepend({
            "--tool=callgrind"sv,
            "--instr-atstart=no"sv,
            ""sv, // Placeholder for the process path.
        });
    }

    if (browser_options.debug_helper_process == process_type)
        arguments.append("--wait-for-debugger"sv);

    for (auto [i, path] : enumerate(candidate_server_paths)) {
        Core::ProcessSpawnOptions options { .name = server_name, .arguments = arguments };

        if (browser_options.profile_helper_process == process_type) {
            options.executable = "valgrind"sv;
            options.search_for_executable_in_path = true;
            arguments[2] = path;
        } else {
            options.executable = path;
        }

        auto result = WebView::Process::spawn<ClientType>(process_type, move(options), forward<ClientArguments>(client_arguments)...);

        if (!result.is_error()) {
            auto&& [process, client] = result.release_value();

            if constexpr (requires { client->set_pid(pid_t {}); })
                client->set_pid(process.pid());

            if constexpr (requires { client->transport().set_peer_pid(0); } && !IsSame<ClientType, Web::HTML::WebWorkerClient>) {
                auto response = client->template send_sync<typename ClientType::InitTransport>(Core::System::getpid());
                client->transport().set_peer_pid(response->peer_pid());
            }

            WebView::Application::the().add_child_process(move(process));

            if (browser_options.profile_helper_process == process_type) {
                dbgln();
                dbgln("\033[1;45mLaunched {} process under callgrind!\033[0m", server_name);
                dbgln("\033[100mRun `\033[4mcallgrind_control -i on\033[24m` to start instrumentation and `\033[4mcallgrind_control -i off\033[24m` stop it again.\033[0m");
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

template<typename... ClientArguments>
static ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process_impl(
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket,
    ClientArguments&&... client_arguments)
{
    auto const& web_content_options = WebView::Application::web_content_options();

    Vector<ByteString> arguments {
        "--command-line"sv,
        web_content_options.command_line.to_byte_string(),
        "--executable-path"sv,
        web_content_options.executable_path.to_byte_string(),
    };

    if (web_content_options.config_path.has_value()) {
        arguments.append("--config-path"sv);
        arguments.append(web_content_options.config_path.value());
    }
    if (web_content_options.is_layout_test_mode == WebView::IsLayoutTestMode::Yes)
        arguments.append("--layout-test-mode"sv);
    if (web_content_options.log_all_js_exceptions == WebView::LogAllJSExceptions::Yes)
        arguments.append("--log-all-js-exceptions"sv);
    if (web_content_options.disable_site_isolation == WebView::DisableSiteIsolation::Yes)
        arguments.append("--disable-site-isolation"sv);
    if (web_content_options.enable_idl_tracing == WebView::EnableIDLTracing::Yes)
        arguments.append("--enable-idl-tracing"sv);
    if (web_content_options.enable_http_cache == WebView::EnableHTTPCache::Yes)
        arguments.append("--enable-http-cache"sv);
    if (web_content_options.expose_internals_object == WebView::ExposeInternalsObject::Yes)
        arguments.append("--expose-internals-object"sv);
    if (web_content_options.force_cpu_painting == WebView::ForceCPUPainting::Yes)
        arguments.append("--force-cpu-painting"sv);
    if (web_content_options.force_fontconfig == WebView::ForceFontconfig::Yes)
        arguments.append("--force-fontconfig"sv);
    if (web_content_options.collect_garbage_on_every_allocation == WebView::CollectGarbageOnEveryAllocation::Yes)
        arguments.append("--collect-garbage-on-every-allocation"sv);
    if (web_content_options.is_headless == WebView::IsHeadless::Yes)
        arguments.append("--headless"sv);
    if (web_content_options.paint_viewport_scrollbars == PaintViewportScrollbars::No)
        arguments.append("--disable-scrollbar-painting"sv);

    if (auto const maybe_echo_server_port = web_content_options.echo_server_port; maybe_echo_server_port.has_value()) {
        arguments.append("--echo-server-port"sv);
        arguments.append(ByteString::number(maybe_echo_server_port.value()));
    }

    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }
    if (request_server_socket.has_value()) {
        arguments.append("--request-server-socket"sv);
        arguments.append(ByteString::number(request_server_socket->fd()));
    }

    arguments.append("--image-decoder-socket"sv);
    arguments.append(ByteString::number(image_decoder_socket.fd()));

    return launch_server_process<WebView::WebContentClient>("WebContent"sv, move(arguments), forward<ClientArguments>(client_arguments)...);
}

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(
    WebView::ViewImplementation& view,
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket)
{
    return launch_web_content_process_impl(move(image_decoder_socket), move(request_server_socket), view);
}

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_spare_web_content_process(
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket)
{
    return launch_web_content_process_impl(move(image_decoder_socket), move(request_server_socket));
}

ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process()
{
    Vector<ByteString> arguments;
    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    return launch_server_process<ImageDecoderClient::Client>("ImageDecoder"sv, arguments);
}

ErrorOr<NonnullRefPtr<Web::HTML::WebWorkerClient>> launch_web_worker_process()
{
    Vector<ByteString> arguments;

    auto request_server_socket = TRY(connect_new_request_server_client());
    arguments.append("--request-server-socket"sv);
    arguments.append(ByteString::number(request_server_socket.fd()));

    auto image_decoder_socket = TRY(connect_new_image_decoder_client());
    arguments.append("--image-decoder-socket"sv);
    arguments.append(ByteString::number(image_decoder_socket.fd()));

    return launch_server_process<Web::HTML::WebWorkerClient>("WebWorker"sv, move(arguments));
}

ErrorOr<NonnullRefPtr<Requests::RequestClient>> launch_request_server_process()
{
    Vector<ByteString> arguments;

    if (!s_ladybird_resource_root.is_empty()) {
        arguments.append("--serenity-resource-root"sv);
        arguments.append(s_ladybird_resource_root);
    }

    for (auto const& certificate : WebView::Application::browser_options().certificates)
        arguments.append(ByteString::formatted("--certificate={}", certificate));

    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    auto client = TRY(launch_server_process<Requests::RequestClient>("RequestServer"sv, move(arguments)));
    WebView::Application::browser_options().dns_settings.visit(
        [](WebView::SystemDNS) {},
        [&](WebView::DNSOverTLS const& dns_over_tls) {
            dbgln("Setting DNS server to {}:{} with TLS", dns_over_tls.server_address, dns_over_tls.port);
            client->async_set_dns_server(dns_over_tls.server_address, dns_over_tls.port, true);
        },
        [&](WebView::DNSOverUDP const& dns_over_udp) {
            dbgln("Setting DNS server to {}:{}", dns_over_udp.server_address, dns_over_udp.port);
            client->async_set_dns_server(dns_over_udp.server_address, dns_over_udp.port, false);
        });

    return client;
}

ErrorOr<IPC::File> connect_new_request_server_client()
{
    auto new_socket = Application::request_server_client().send_sync_but_allow_failure<Messages::RequestServer::ConnectNewClient>();
    if (!new_socket)
        return Error::from_string_literal("Failed to connect to RequestServer");

    auto socket = new_socket->take_client_socket();
    TRY(socket.clear_close_on_exec());

    return socket;
}

ErrorOr<IPC::File> connect_new_image_decoder_client()
{
    auto new_socket = Application::image_decoder_client().send_sync_but_allow_failure<Messages::ImageDecoderServer::ConnectNewClients>(1);
    if (!new_socket)
        return Error::from_string_literal("Failed to connect to ImageDecoder");

    auto sockets = new_socket->take_sockets();
    if (sockets.size() != 1)
        return Error::from_string_literal("Failed to connect to ImageDecoder");

    auto socket = sockets.take_last();
    TRY(socket.clear_close_on_exec());

    return socket;
}

}
