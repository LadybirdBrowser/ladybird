/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Worker/WebWorkerClient.h>

namespace Web::HTML {

void WebWorkerClient::die()
{
    // FIXME: Notify WorkerAgent that the worker is dead
}

void WebWorkerClient::did_close_worker()
{
    if (on_worker_close)
        on_worker_close();
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

Messages::WebWorkerClient::RequestWorkerAgentResponse WebWorkerClient::request_worker_agent(Web::Bindings::AgentType worker_type)
{
    if (on_request_worker_agent)
        return on_request_worker_agent(worker_type);
    return { IPC::TransportHandle {}, IPC::TransportHandle {}, IPC::TransportHandle {} };
}

WebWorkerClient::WebWorkerClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
{
}

}
