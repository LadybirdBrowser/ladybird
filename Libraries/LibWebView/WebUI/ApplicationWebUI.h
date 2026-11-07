/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/WebUI.h>

namespace WebView {

// A page that the application answers, rather than anything in this process. What goes between the page and the
// application passes through here without being looked at.
class WEBVIEW_API ApplicationWebUI final : public WebUI {
    WEB_UI(ApplicationWebUI);

public:
    struct Callbacks {
        Function<void(u64 id, String const& host, IsPrivate)> on_open;
        Function<void(u64 id, String name, JsonValue data)> on_message;
        Function<void(u64 id)> on_close;
    };
    static void set_callbacks(Callbacks);

    static ApplicationWebUI* find(u64 id);

    virtual ~ApplicationWebUI() override;

    void send_to_page(String const& name, JsonValue const& data);

    // NB: Some actions of a page, such as opening a download, need the user's own click in it.
    bool consume_user_activation();

private:
    virtual void register_interfaces() override;
    virtual void received_message(String name, JsonValue data) override;

    u64 m_id { 0 };
};

}
