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
    explicit WebContentConnection(NonnullOwnPtr<IPC::Transport> transport);

    Web::WebDriver::Response wait_for_navigation_completion()
    {
        auto response = send_sync_but_allow_failure<Messages::WebDriverClient::WaitForNavigationCompletion>();
        VERIFY(response);
        return response->response();
    }

    Function<void()> on_close;
    Function<void(Web::WebDriver::Response)> on_driver_execution_complete;
    Function<void(String)> on_did_set_window_handle;
    Function<void(String)> on_did_start_window_replacement;
    Function<void(String)> on_did_close_window;

private:
    virtual void die() override;

    virtual void driver_execution_complete(Web::WebDriver::Response) override;
    virtual void did_set_window_handle(String) override;
    virtual void did_start_window_replacement(String) override;
    virtual void did_close_window(String) override;
};

}
