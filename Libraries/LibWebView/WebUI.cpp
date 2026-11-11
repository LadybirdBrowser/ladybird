/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>
#include <LibWebView/WebUI/ApplicationWebUI.h>
#include <LibWebView/WebUI/CrashReportsUI.h>
#include <LibWebView/WebUI/SettingsUI.h>

namespace WebView {

static constexpr auto s_pages = to_array<WebUI::Page>({
    { "about"sv, "About URLs"sv, WebUI::PageType::Static },
    { "blocking"sv, "Blocking"sv, WebUI::PageType::Static },
    { "crashes"sv, "Crash Reports"sv, WebUI::PageType::Dynamic },
    { "newtab"sv, "New Tab"sv, WebUI::PageType::Static },
    { "settings"sv, "Settings"sv, WebUI::PageType::Dynamic },
    { "welcome"sv, "Welcome"sv, WebUI::PageType::Dynamic },
});

struct ApplicationPage {
    String host;
    String title;
};

static Vector<ApplicationPage>& application_pages()
{
    static NeverDestroyed<Vector<ApplicationPage>> s_application_pages;
    return *s_application_pages;
}

Vector<WebUI::Page> WebUI::pages()
{
    Vector<Page> pages;
    pages.ensure_capacity(s_pages.size() + application_pages().size());
    for (auto const& page : s_pages)
        pages.unchecked_append(page);
    for (auto const& page : application_pages())
        pages.unchecked_append({ page.host, page.title, PageType::Application });
    return pages;
}

Optional<WebUI::Page> WebUI::page_for_host(StringView host)
{
    for (auto const& page : s_pages) {
        if (page.host == host)
            return page;
    }
    for (auto const& page : application_pages()) {
        if (page.host == host)
            return Page { page.host, page.title, PageType::Application };
    }
    return {};
}

void WebUI::add_application_page(String host, String title)
{
    if (!page_for_host(host).has_value())
        application_pages().append({ move(host), move(title) });
}

template<typename WebUIType>
static ErrorOr<NonnullRefPtr<WebUIType>> create_web_ui(WebContentClient& client, Web::PageId page_id, String host)
{
    VERIFY(page_id > 0);

    auto paired = TRY(IPC::Transport::create_paired());
    auto handle = move(paired.remote_handle);

    auto web_ui = WebUIType::create(client, page_id, move(paired.local), move(host));
    client.async_connect_to_web_ui(page_id, move(handle));

    return web_ui;
}

ErrorOr<RefPtr<WebUI>> WebUI::create(WebContentClient& client, Web::PageId page_id, String host)
{
    auto page = page_for_host(host);
    if (!page.has_value() || page->type == PageType::Static)
        return nullptr;

    RefPtr<WebUI> web_ui;

    if (page->type == PageType::Application)
        web_ui = TRY(create_web_ui<ApplicationWebUI>(client, page_id, move(host)));
    else if (page->host == "crashes"sv)
        web_ui = TRY(create_web_ui<CrashReportsUI>(client, page_id, move(host)));
    else if (page->host.is_one_of("settings"sv, "welcome"sv))
        web_ui = TRY(create_web_ui<SettingsUI>(client, page_id, move(host)));

    VERIFY(web_ui);
    web_ui->register_interfaces();

    return web_ui;
}

WebUI::WebUI(WebContentClient& client, Web::PageId page_id, NonnullOwnPtr<IPC::Transport> transport, String host)
    : IPC::ConnectionToServer<WebUIClientEndpoint, WebUIServerEndpoint>(*this, move(transport))
    , m_client(client)
    , m_page_id(page_id)
    , m_host(move(host))
{
}

WebUI::~WebUI() = default;

void WebUI::die()
{
    if (auto client = m_client.strong_ref())
        if (auto* page = client->page(m_page_id))
            page->web_ui_disconnected({}, *this);
}

void WebUI::register_interface(StringView name, Interface interface)
{
    auto result = m_interfaces.set(name, move(interface));
    VERIFY(result == HashSetResult::InsertedNewEntry);
}

void WebUI::received_message(String name, JsonValue data)
{
    if (!has_client())
        return;

    auto interface = m_interfaces.get(name);
    if (!interface.has_value()) {
        warnln("Received message from WebUI for unrecognized interface: {}", name);
        return;
    }

    interface.value()(move(data));
}

}
