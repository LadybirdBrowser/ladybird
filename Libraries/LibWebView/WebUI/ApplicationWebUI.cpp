/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/ApplicationWebUI.h>

namespace WebView {

static ApplicationWebUI::Callbacks& callbacks()
{
    static NeverDestroyed<ApplicationWebUI::Callbacks> s_callbacks;
    return *s_callbacks;
}

static HashMap<u64, ApplicationWebUI*>& web_uis()
{
    static NeverDestroyed<HashMap<u64, ApplicationWebUI*>> s_web_uis;
    return *s_web_uis;
}

static u64 s_next_id { 1 };

void ApplicationWebUI::set_callbacks(Callbacks callbacks)
{
    ::WebView::callbacks() = move(callbacks);
}

ApplicationWebUI* ApplicationWebUI::find(u64 id)
{
    return web_uis().get(id).value_or(nullptr);
}

ApplicationWebUI::~ApplicationWebUI()
{
    web_uis().remove(m_id);
    if (callbacks().on_close)
        callbacks().on_close(m_id);
}

void ApplicationWebUI::register_interfaces()
{
    m_id = s_next_id++;
    web_uis().set(m_id, this);
    if (callbacks().on_open)
        callbacks().on_open(m_id, host(), client().is_private());
}

void ApplicationWebUI::received_message(String name, JsonValue data)
{
    if (has_client() && callbacks().on_message)
        callbacks().on_message(m_id, move(name), move(data));
}

bool ApplicationWebUI::consume_user_activation()
{
    if (!has_client())
        return false;
    auto* page = client().page(page_id());
    return page && page->consume_transient_user_activation();
}

void ApplicationWebUI::send_to_page(String const& name, JsonValue const& data)
{
    if (has_client())
        async_send_message(name, data);
}

}
