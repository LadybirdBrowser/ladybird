/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Bookmarks.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/RemoteSuggestionProvider.h>

namespace Ladybird {

static HashMap<u64, RemoteSuggestionProvider*> s_providers;
static u64 s_next_client_id { 1 };

RemoteSuggestionProvider::RemoteSuggestionProvider(WebView::IsPrivate is_private)
    : m_client_id(s_next_client_id++)
{
    s_providers.set(m_client_id, this);
    HubConnection::the().async_omnibox_open(m_client_id, is_private);
}

RemoteSuggestionProvider::~RemoteSuggestionProvider()
{
    s_providers.remove(m_client_id);
    if (auto* hub = HubConnection::the_if_connected())
        hub->async_omnibox_close(m_client_id);
}

RemoteSuggestionProvider* RemoteSuggestionProvider::find(u64 client_id)
{
    return s_providers.get(client_id).value_or(nullptr);
}

void RemoteSuggestionProvider::query(WebView::AutocompleteQueryID query_id, String query, size_t max_suggestions)
{
    auto bookmarks = Application::bookmarks().suggestions_for(query, max_suggestions);
    HubConnection::the().async_omnibox_query(m_client_id, query_id, move(query), max_suggestions, move(bookmarks));
}

void RemoteSuggestionProvider::cancel()
{
    HubConnection::the().async_omnibox_cancel(m_client_id);
}

void RemoteSuggestionProvider::record_engagement(WebView::OmniboxEngagement engagement)
{
    HubConnection::the().async_omnibox_record_engagement(m_client_id, move(engagement));
}

}
