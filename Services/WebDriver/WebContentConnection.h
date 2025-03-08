/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
    explicit WebContentConnection(IPC::Transport transport);

    Function<void()> on_close;
    Function<void(Web::WebDriver::Response)> on_driver_execution_complete;

private:
    virtual void die() override;

    virtual void driver_execution_complete(Web::WebDriver::Response) override;
};

}
