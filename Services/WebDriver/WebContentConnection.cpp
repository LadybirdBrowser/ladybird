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

void WebContentConnection::driver_execution_complete(Web::WebDriver::Response response)
{
    if (on_driver_execution_complete)
        on_driver_execution_complete(move(response));
}

}
