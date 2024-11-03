/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWeb/Worker/WebWorkerClientEndpoint.h>
#include <LibWeb/Worker/WebWorkerServerEndpoint.h>

namespace Web::HTML {

class WebWorkerClient final
    : public IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>
    , public WebWorkerClientEndpoint {
    IPC_CLIENT_CONNECTION(WebWorkerClient, "/tmp/session/%sid/portal/webworker"sv);

public:
    explicit WebWorkerClient(IPC::Transport);

    virtual void did_close_worker() override;

    Function<void()> on_worker_close;

    IPC::File clone_transport();

private:
    virtual void die() override;
};

}
