/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Application.h>
#include <UI/Qt/Bookmarks.h>
#include <UI/Qt/SuggestionProvider.h>

namespace Ladybird {

SuggestionProvider::SuggestionProvider(WebView::IsPrivate is_private)
    : m_autocomplete(Application::the(), is_private)
{
    m_autocomplete.on_autocomplete_query_complete = [this](auto query_id, auto suggestions, auto) {
        if (on_suggestions)
            on_suggestions(query_id, move(suggestions));
    };
}

void SuggestionProvider::query(WebView::AutocompleteQueryID query_id, String query, size_t max_suggestions)
{
    auto bookmarks = Application::bookmarks().suggestions_for(query, max_suggestions);
    m_autocomplete.query_autocomplete_engine(query_id, move(query), max_suggestions, move(bookmarks));
}

void SuggestionProvider::cancel()
{
    m_autocomplete.cancel_pending_query();
}

void SuggestionProvider::record_engagement(WebView::OmniboxEngagement engagement)
{
    m_autocomplete.record_engagement(move(engagement));
}

}
