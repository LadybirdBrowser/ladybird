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
    Function<void(Web::WebDriver::Response)> on_window_rect_updated;
    Function<void(Web::WebDriver::Response)> on_find_elements_complete;
    Function<void(Web::WebDriver::Response)> on_script_executed;
    Function<void(Web::WebDriver::Response)> on_actions_performed;
    Function<void(Web::WebDriver::Response)> on_dialog_closed;
    Function<void(Web::WebDriver::Response)> on_screenshot_taken;

private:
    virtual void die() override;

    virtual void navigation_complete(Web::WebDriver::Response const&) override;
    virtual void window_rect_updated(Web::WebDriver::Response const&) override;
    virtual void find_elements_complete(Web::WebDriver::Response const&) override;
    virtual void script_executed(Web::WebDriver::Response const&) override;
    virtual void actions_performed(Web::WebDriver::Response const&) override;
    virtual void dialog_closed(Web::WebDriver::Response const&) override;
    virtual void screenshot_taken(Web::WebDriver::Response const&) override;
};

}
