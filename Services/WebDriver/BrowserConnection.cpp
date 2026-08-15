/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebDriver/BrowserConnection.h>

namespace WebDriver {

BrowserConnection::BrowserConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<WebDriverBrowserClientEndpoint, WebDriverBrowserServerEndpoint>(*this, move(transport), 1)
{
}

void BrowserConnection::die()
{
    auto protector = NonnullRefPtr { *this };

    if (on_close)
        on_close();
}

void BrowserConnection::did_create_window(String handle)
{
    if (on_did_create_window)
        on_did_create_window(move(handle));
}

void BrowserConnection::did_close_window(String handle)
{
    if (on_did_close_window)
        on_did_close_window(move(handle));
}

void BrowserConnection::command_complete(u64 command_id, Web::WebDriver::Response response)
{
    if (on_command_complete)
        on_command_complete(command_id, move(response));
}

}
