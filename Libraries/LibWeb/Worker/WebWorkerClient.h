/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/IterationDecision.h>
#include <AK/Types.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/BroadcastChannelMessage.h>
#include <LibWeb/Worker/WebWorkerClientEndpoint.h>
#include <LibWeb/Worker/WebWorkerServerEndpoint.h>

namespace Web::HTML {

class WEB_API WebWorkerClient final
    : public IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>
    , public WebWorkerClientEndpoint {
    C_OBJECT_ABSTRACT(WebWorkerClient);

public:
    template<typename Callback>
    static void for_each_client(Callback callback);

    explicit WebWorkerClient(NonnullOwnPtr<IPC::Transport>);
    ~WebWorkerClient();

    pid_t pid() const { return m_pid; }
    void set_pid(pid_t pid) { m_pid = pid; }

    virtual void did_close_worker() override;
    virtual void did_fail_loading_worker_script() override;
    virtual void did_report_worker_exception(String message, String filename, u32 lineno, u32 colno) override;
    virtual Messages::WebWorkerClient::DidRequestCookieResponse did_request_cookie(URL::URL, HTTP::Cookie::Source) override;
    virtual void did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage) override;
    virtual Messages::WebWorkerClient::RequestWorkerAgentResponse request_worker_agent(Web::Bindings::AgentType worker_type) override;

    Function<void()> on_worker_close;
    Function<void()> on_worker_script_load_failure;
    Function<void(String, String, u32, u32)> on_worker_exception;
    Function<HTTP::Cookie::VersionedCookie(URL::URL const&, HTTP::Cookie::Source)> on_request_cookie;
    Function<void(Web::HTML::BroadcastChannelMessage)> on_post_broadcast_channel_message;
    Function<Messages::WebWorkerClient::RequestWorkerAgentResponse(Web::Bindings::AgentType)> on_request_worker_agent;

private:
    virtual void die() override;

    pid_t m_pid { -1 };
    static HashTable<WebWorkerClient*> s_all_clients;
};

template<typename Callback>
void WebWorkerClient::for_each_client(Callback callback)
{
    for (auto* client : s_all_clients) {
        if (callback(*client) == IterationDecision::Break)
            return;
    }
}

}
