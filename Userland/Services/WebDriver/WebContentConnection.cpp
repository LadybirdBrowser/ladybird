/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebDriver/Client.h>
#include <WebDriver/WebContentConnection.h>

namespace WebDriver {

WebContentConnection::WebContentConnection(NonnullOwnPtr<Core::LocalSocket> socket)
    : IPC::ConnectionFromClient<WebDriverClientEndpoint, WebDriverServerEndpoint>(*this, move(socket), 1)
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

}
