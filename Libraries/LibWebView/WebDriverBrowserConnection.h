/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <WebDriver/WebDriverBrowserClientEndpoint.h>
#include <WebDriver/WebDriverBrowserServerEndpoint.h>

namespace WebView {

class WebDriverBrowserConnection final
    : public IPC::ConnectionToServer<WebDriverBrowserClientEndpoint, WebDriverBrowserServerEndpoint> {
    C_OBJECT_ABSTRACT(WebDriverBrowserConnection)
public:
    static ErrorOr<NonnullRefPtr<WebDriverBrowserConnection>> connect(ByteString const& webdriver_endpoint);

private:
    explicit WebDriverBrowserConnection(NonnullOwnPtr<IPC::Transport>);

    virtual void die() override;

    virtual void close_session() override;
    virtual void traverse_history(u64 command_id, String window_handle, i32 delta, bool handle_user_prompts) override;
    virtual void get_session_history(u64 command_id, String window_handle) override;
};

}
