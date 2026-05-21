/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Worker/WebWorkerClient.h>

namespace Web::HTML {

HashTable<WebWorkerClient*> WebWorkerClient::s_all_clients;

void WebWorkerClient::die()
{
    if (on_worker_died)
        on_worker_died();
}

void WebWorkerClient::did_close_worker()
{
    if (on_worker_close)
        on_worker_close();
}

void WebWorkerClient::did_finish_loading_worker_script(bool worker_is_secure_context)
{
    if (on_worker_script_load_success)
        on_worker_script_load_success(worker_is_secure_context);
}

void WebWorkerClient::did_fail_loading_worker_script()
{
    if (on_worker_script_load_failure)
        on_worker_script_load_failure();
}

void WebWorkerClient::did_report_worker_exception(String message, String filename, u32 lineno, u32 colno)
{
    if (on_worker_exception)
        on_worker_exception(move(message), move(filename), lineno, colno);
}

Messages::WebWorkerClient::DidRequestCookieResponse WebWorkerClient::did_request_cookie(URL::URL url, HTTP::Cookie::Source source)
{
    if (on_request_cookie)
        return on_request_cookie(url, source);
    return HTTP::Cookie::VersionedCookie {};
}

void WebWorkerClient::did_request_file(ByteString path, i32 request_id)
{
    if (on_request_file)
        on_request_file(move(path), request_id);
}

void WebWorkerClient::did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage message)
{
    if (on_post_broadcast_channel_message)
        on_post_broadcast_channel_message(move(message));
}

Messages::WebWorkerClient::StartWorkerAgentResponse WebWorkerClient::start_worker_agent(Web::HTML::WorkerAgentStartRequest request)
{
    if (on_start_worker_agent)
        return on_start_worker_agent(move(request));
    return { 0 };
}

void WebWorkerClient::close_worker_agent(Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    if (on_close_worker_agent)
        on_close_worker_agent(agent_id, owner_token);
}

WebWorkerClient::WebWorkerClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
{
    s_all_clients.set(this);
}

WebWorkerClient::~WebWorkerClient()
{
    s_all_clients.remove(this);
}

}
