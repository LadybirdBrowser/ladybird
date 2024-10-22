/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebDriver/Client.h>
#include <WebDriver/WebContentConnection.h>

namespace WebDriver {

WebContentConnection::WebContentConnection(IPC::Transport transport)
    : IPC::ConnectionFromClient<WebDriverClientEndpoint, WebDriverServerEndpoint>(*this, move(transport), 1)
{
}

void WebContentConnection::die()
{
    if (on_close)
        on_close();
}

void WebContentConnection::script_executed(Web::WebDriver::Response const& response)
{
    if (on_script_executed)
        on_script_executed(response);
}

void WebContentConnection::actions_performed(Web::WebDriver::Response const& response)
{
    if (on_actions_performed)
        on_actions_performed(response);
}

}
