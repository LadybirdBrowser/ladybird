/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IDAllocator.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Transport.h>
#include <WebContent/WebDriverClientEndpoint.h>
#include <WebContent/WebDriverServerEndpoint.h>

namespace WebDriver {

class Client;

class WebContentConnection
    : public IPC::ConnectionFromClient<WebDriverClientEndpoint, WebDriverServerEndpoint> {
    C_OBJECT_ABSTRACT(WebContentConnection)
public:
    explicit WebContentConnection(NonnullOwnPtr<IPC::Transport> transport);

    Function<void()> on_close;

    int create_pending_request(NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> promise);

private:
    virtual void die() override;
    virtual void driver_execution_complete(int, Web::WebDriver::Response) override;

    IDAllocator m_request_id_allocator;
    HashMap<int, NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>>> m_pending_requests;
};

}
