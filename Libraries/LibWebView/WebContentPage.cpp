/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

WebContentPage::WebContentPage(WebContentClient& client, Web::PageId id, CanonicalTraversable& traversable, ViewImplementation* view)
    : m_client(client)
    , m_id(id)
    , m_traversable(traversable.make_weak_ptr<CanonicalTraversable>())
    , m_view(view)
{
}

WebContentPage::~WebContentPage() = default;

WebContentClient& WebContentPage::client() const
{
    VERIFY(m_client);
    return *m_client;
}

bool WebContentPage::is_live() const
{
    return m_client && m_client->is_page_open(m_id);
}

void WebContentPage::close()
{
    m_is_open = false;
    m_traversable = nullptr;
    m_view = nullptr;
    m_needs_beforeunload_check = true;
    m_history_recorded_url_for_current_load.clear();
}

CanonicalTraversable* WebContentPage::traversable() const
{
    if (!is_open())
        return nullptr;
    return m_traversable.ptr();
}

Optional<ViewImplementation&> WebContentPage::view() const
{
    if (!is_open() || !m_view)
        return {};
    return *m_view;
}

Optional<ViewImplementation&> WebContentPage::owning_view() const
{
    auto* traversable = this->traversable();
    if (!traversable)
        return {};
    return ViewImplementation::find_view_for_traversable(*traversable);
}

Optional<CanonicalNavigable&> WebContentPage::hosted_navigable(Web::HTML::CrossProcessId navigable_id) const
{
    auto* traversable = this->traversable();
    if (!traversable)
        return {};
    auto navigable = traversable->find(navigable_id);
    if (!navigable.has_value() || !traversable->hosts(*navigable, *this))
        return {};
    return *navigable;
}

}
