/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibHTTP/Cookie/Cookie.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Export.h>
#include <LibWeb/Worker/WebWorkerClientEndpoint.h>
#include <LibWeb/Worker/WebWorkerServerEndpoint.h>

namespace Web::HTML {

class WEB_API WebWorkerClient final
    : public IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>
    , public WebWorkerClientEndpoint {
    C_OBJECT_ABSTRACT(WebWorkerClient);

public:
    explicit WebWorkerClient(NonnullOwnPtr<IPC::Transport>);

    virtual void did_close_worker() override;
    virtual void did_fail_loading_worker_script() override;
    virtual Messages::WebWorkerClient::DidRequestCookieResponse did_request_cookie(URL::URL, HTTP::Cookie::Source) override;
    virtual Messages::WebWorkerClient::RequestWorkerAgentResponse request_worker_agent(Web::Bindings::AgentType worker_type) override;

    Function<void()> on_worker_close;
    Function<void()> on_worker_script_load_failure;
    Function<HTTP::Cookie::VersionedCookie(URL::URL const&, HTTP::Cookie::Source)> on_request_cookie;
    Function<Messages::WebWorkerClient::RequestWorkerAgentResponse(Web::Bindings::AgentType)> on_request_worker_agent;

private:
    virtual void die() override;
};

}
