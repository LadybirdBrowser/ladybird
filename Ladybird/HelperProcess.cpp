/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HelperProcess.h"
#include "Utilities.h"
#include <AK/Enumerate.h>
#include <LibCore/Process.h>
#include <LibWebView/Application.h>

template<typename ClientType, typename... ClientArguments>
static ErrorOr<NonnullRefPtr<ClientType>> launch_server_process(
    StringView server_name,
    ReadonlySpan<ByteString> candidate_server_paths,
    Vector<ByteString> arguments,
    ClientArguments&&... client_arguments)
{
    auto process_type = WebView::process_type_from_name(server_name);
    auto const& chrome_options = WebView::Application::chrome_options();

    if (chrome_options.profile_helper_process == process_type) {
        arguments.prepend({
            "--tool=callgrind"sv,
            "--instr-atstart=no"sv,
            ""sv, // Placeholder for the process path.
        });
    }

    if (chrome_options.debug_helper_process == process_type)
        arguments.append("--wait-for-debugger"sv);

    for (auto [i, path] : enumerate(candidate_server_paths)) {
        Core::ProcessSpawnOptions options { .name = server_name, .arguments = arguments };

        if (chrome_options.profile_helper_process == process_type) {
            options.executable = "valgrind"sv;
            options.search_for_executable_in_path = true;
            arguments[2] = path;
        } else {
            options.executable = path;
        }

        auto result = Core::IPCProcess::spawn<ClientType>(move(options), forward<ClientArguments>(client_arguments)...);

        if (!result.is_error()) {
            auto process = result.release_value();

            if constexpr (requires { process.client->set_pid(pid_t {}); })
                process.client->set_pid(process.process.pid());

            WebView::Application::the().add_child_process(WebView::Process { process_type, process.client, move(process.process) });

            if (chrome_options.profile_helper_process == process_type) {
                dbgln();
                dbgln("\033[1;45mLaunched {} process under callgrind!\033[0m", server_name);
                dbgln("\033[100mRun `\033[4mcallgrind_control -i on\033[24m` to start instrumentation and `\033[4mcallgrind_control -i off\033[24m` stop it again.\033[0m");
                dbgln();
            }

            return move(process.client);
        }

        if (i == candidate_server_paths.size() - 1) {
            warnln("Could not launch any of {}: {}", candidate_server_paths, result.error());
            return result.release_error();
        }
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(
    WebView::ViewImplementation& view,
    ReadonlySpan<ByteString> candidate_web_content_paths,
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket)
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

    return launch_server_process<WebView::WebContentClient>("WebContent"sv, candidate_web_content_paths, move(arguments), view);
}

ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process(ReadonlySpan<ByteString> candidate_image_decoder_paths)
{
    Vector<ByteString> arguments;
    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    return launch_server_process<ImageDecoderClient::Client>("ImageDecoder"sv, candidate_image_decoder_paths, arguments);
}

ErrorOr<NonnullRefPtr<Web::HTML::WebWorkerClient>> launch_web_worker_process(ReadonlySpan<ByteString> candidate_web_worker_paths, NonnullRefPtr<Requests::RequestClient> request_client)
{
    Vector<ByteString> arguments;

    auto socket = TRY(connect_new_request_server_client(*request_client));
    arguments.append("--request-server-socket"sv);
    arguments.append(ByteString::number(socket.fd()));

    return launch_server_process<Web::HTML::WebWorkerClient>("WebWorker"sv, candidate_web_worker_paths, move(arguments));
}

ErrorOr<NonnullRefPtr<Requests::RequestClient>> launch_request_server_process(ReadonlySpan<ByteString> candidate_request_server_paths, StringView serenity_resource_root)
{
    Vector<ByteString> arguments;

    if (!serenity_resource_root.is_empty()) {
        arguments.append("--serenity-resource-root"sv);
        arguments.append(serenity_resource_root);
    }

    for (auto const& certificate : WebView::Application::chrome_options().certificates)
        arguments.append(ByteString::formatted("--certificate={}", certificate));

    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    return launch_server_process<Requests::RequestClient>("RequestServer"sv, candidate_request_server_paths, move(arguments));
}

ErrorOr<IPC::File> connect_new_request_server_client(Requests::RequestClient& client)
{
    auto new_socket = client.send_sync_but_allow_failure<Messages::RequestServer::ConnectNewClient>();
    if (!new_socket)
        return Error::from_string_literal("Failed to connect to RequestServer");

    auto socket = new_socket->take_client_socket();
    TRY(socket.clear_close_on_exec());

    return socket;
}

ErrorOr<IPC::File> connect_new_image_decoder_client(ImageDecoderClient::Client& client)
{
    auto new_socket = client.send_sync_but_allow_failure<Messages::ImageDecoderServer::ConnectNewClients>(1);
    if (!new_socket)
        return Error::from_string_literal("Failed to connect to ImageDecoder");

    auto sockets = new_socket->take_sockets();
    if (sockets.size() != 1)
        return Error::from_string_literal("Failed to connect to ImageDecoder");

    auto socket = sockets.take_last();
    TRY(socket.clear_close_on_exec());

    return socket;
}
