/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/HTML/BroadcastChannelMessage.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>
#include <LibWeb/Worker/WebWorkerClientEndpoint.h>
#include <LibWeb/Worker/WebWorkerServerEndpoint.h>
#include <LibWebView/Export.h>

namespace WebView {

class WEBVIEW_API WebWorkerClient final
    : public IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>
    , public WebWorkerClientEndpoint {
    C_OBJECT_ABSTRACT(WebWorkerClient);

public:
    using InitTransport = Messages::WebWorkerServer::InitTransport;

    explicit WebWorkerClient(NonnullOwnPtr<IPC::Transport>, Web::HTML::WorkerAgentId agent_id);
    ~WebWorkerClient();

    pid_t pid() const { return m_pid; }
    void set_pid(pid_t pid) { m_pid = pid; }

    virtual void did_close_worker() override;
    virtual void did_finish_loading_worker_script(bool worker_is_secure_context) override;
    virtual void did_fail_loading_worker_script() override;
    virtual void did_report_worker_exception(String message, String filename, u32 lineno, u32 colno) override;
    virtual Messages::WebWorkerClient::DidRequestCookieResponse did_request_cookie(URL::URL, HTTP::Cookie::Source) override;
    virtual void did_request_file(ByteString path, i32 request_id) override;
    virtual void did_store_hsts_policy(String domain, HTTP::HSTS::ParsedHSTSPolicy policy) override;
    virtual Messages::WebWorkerClient::DidIsKnownHstsHostResponse did_is_known_hsts_host(String domain) override;
    virtual void did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage) override;
    virtual Messages::WebWorkerClient::StartWorkerAgentResponse start_worker_agent(Web::HTML::WorkerAgentStartRequest request) override;
    virtual void close_worker_agent(Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken) override;

private:
    virtual void die() override;

    pid_t m_pid { -1 };
    Web::HTML::WorkerAgentId m_agent_id { 0 };
};

}
