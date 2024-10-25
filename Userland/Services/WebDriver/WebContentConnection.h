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
    Function<void(Web::WebDriver::Response)> on_navigation_complete;
    Function<void(Web::WebDriver::Response)> on_script_executed;
    Function<void(Web::WebDriver::Response)> on_actions_performed;

private:
    virtual void die() override;

    virtual void navigation_complete(Web::WebDriver::Response const&) override;
    virtual void script_executed(Web::WebDriver::Response const&) override;
    virtual void actions_performed(Web::WebDriver::Response const&) override;
};

}
