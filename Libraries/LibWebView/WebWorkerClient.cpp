/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

void WebWorkerClient::die()
{
    WorkerProcessManager::the().worker_did_die(m_agent_id);

    // Otherwise nested workers we own would outlive us, in violation of the HTML spec.
    WorkerProcessManager::the().remove_web_worker_owner(*this);
}

void WebWorkerClient::did_close_worker()
{
    WorkerProcessManager::the().worker_did_close(m_agent_id);
}

void WebWorkerClient::did_finish_loading_worker_script(bool worker_is_secure_context)
{
    WorkerProcessManager::the().worker_did_finish_loading_script(m_agent_id, worker_is_secure_context);
}

void WebWorkerClient::did_fail_loading_worker_script()
{
    WorkerProcessManager::the().worker_did_fail_loading_script(m_agent_id);
}

void WebWorkerClient::did_report_worker_exception(String message, String filename, u32 lineno, u32 colno)
{
    WorkerProcessManager::the().worker_did_report_exception(m_agent_id, move(message), move(filename), lineno, colno);
}

Messages::WebWorkerClient::DidRequestCookieResponse WebWorkerClient::did_request_cookie(URL::URL url, HTTP::Cookie::Source source)
{
    HTTP::Cookie::VersionedCookie cookie;
    cookie.cookie = Application::cookie_jar().get_cookie(url, source);
    return cookie;
}

void WebWorkerClient::did_request_file(ByteString path, i32 request_id)
{
    WorkerProcessManager::the().worker_did_request_file(m_agent_id, move(path), request_id);
}

void WebWorkerClient::did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage message)
{
    WorkerProcessManager::the().worker_did_post_broadcast_channel_message(m_agent_id, move(message));
}

Messages::WebWorkerClient::StartWorkerAgentResponse WebWorkerClient::start_worker_agent(Web::HTML::WorkerAgentStartRequest request)
{
    return WorkerProcessManager::the().start_worker_agent(*this, move(request));
}

void WebWorkerClient::close_worker_agent(Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    WorkerProcessManager::the().close_worker_agent(*this, agent_id, owner_token);
}

WebWorkerClient::WebWorkerClient(NonnullOwnPtr<IPC::Transport> transport, Web::HTML::WorkerAgentId agent_id)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
    , m_agent_id(agent_id)
{
}

WebWorkerClient::~WebWorkerClient() = default;

}
