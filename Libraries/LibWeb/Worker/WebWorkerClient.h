/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionToServer.h>
#include <LibWeb/Cookie/Cookie.h>
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
    virtual Messages::WebWorkerClient::DidRequestCookieResponse did_request_cookie(URL::URL, Cookie::Source) override;

    Function<void()> on_worker_close;
    Function<String(URL::URL const&, Cookie::Source)> on_request_cookie;

    IPC::File clone_transport();

private:
    virtual void die() override;
};

}
