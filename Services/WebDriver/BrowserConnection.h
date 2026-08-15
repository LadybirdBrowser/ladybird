/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Transport.h>
#include <LibWeb/WebDriver/Response.h>
#include <WebDriver/WebDriverBrowserClientEndpoint.h>
#include <WebDriver/WebDriverBrowserServerEndpoint.h>

namespace WebDriver {

class BrowserConnection
    : public IPC::ConnectionFromClient<WebDriverBrowserClientEndpoint, WebDriverBrowserServerEndpoint> {
    C_OBJECT_ABSTRACT(BrowserConnection)
public:
    explicit BrowserConnection(NonnullOwnPtr<IPC::Transport>);

    Function<void()> on_close;
    Function<void(String)> on_did_create_window;
    Function<void(String)> on_did_close_window;
    Function<void(u64, Web::WebDriver::Response)> on_command_complete;

private:
    virtual void die() override;

    virtual void did_create_window(String) override;
    virtual void did_close_window(String) override;
    virtual void command_complete(u64 command_id, Web::WebDriver::Response) override;
};

}
