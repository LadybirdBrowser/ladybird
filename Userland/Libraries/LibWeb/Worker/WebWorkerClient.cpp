/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibWeb/Worker/WebWorkerClient.h>

namespace Web::HTML {

void WebWorkerClient::die()
{
    // FIXME: Notify WorkerAgent that the worker is ded
}

void WebWorkerClient::did_close_worker()
{
    if (on_worker_close)
        on_worker_close();
}

WebWorkerClient::WebWorkerClient(IPC::Transport transport)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
{
}

IPC::File WebWorkerClient::clone_transport()
{
    return MUST(m_transport.clone_for_transfer());
}

}
