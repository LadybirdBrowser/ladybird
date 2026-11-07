/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/WebUI.h>
#include <LibWebView/WebUI/ApplicationWebUI.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/WebUIPage.h>

namespace Ladybird {

void WebUIPage::send_message(String name, JsonValue data) const
{
    if (auto* hub = HubConnection::the_if_connected())
        hub->async_send_web_ui_message(m_web_ui_id, move(name), move(data));
}

bool WebUIPage::consume_user_activation() const
{
#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
    auto* hub = HubConnection::the_if_connected();
    return hub && hub->web_ui_consume_user_activation(m_web_ui_id);
#else
    auto* web_ui = WebView::ApplicationWebUI::find(m_web_ui_id);
    return web_ui && web_ui->consume_user_activation();
#endif
}

void WebUIPages::register_page(String host, String title, Factory factory)
{
    // NB: The omnibox of this process suggests the pages too.
    WebView::WebUI::add_application_page(host, title);
    if (auto* hub = HubConnection::the_if_connected())
        hub->async_add_web_ui_page(host, move(title));
    m_factories.set(move(host), move(factory));
}

void WebUIPages::page_opened(u64 web_ui_id, String const& host, WebView::IsPrivate is_private)
{
    if (auto factory = m_factories.get(host); factory.has_value())
        m_pages.set(web_ui_id, (*factory)(web_ui_id, is_private));
}

void WebUIPages::page_received_message(u64 web_ui_id, String const& name, JsonValue const& data)
{
    if (auto* page = m_pages.get(web_ui_id).value_or(nullptr))
        page->received_message(name, data);
}

void WebUIPages::page_closed(u64 web_ui_id)
{
    m_pages.remove(web_ui_id);
}

}
