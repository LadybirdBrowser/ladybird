/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Autocomplete.h>
#include <LibWebView/Omnibox.h>

namespace Ladybird {

// Makes the suggestions of a location bar from the browsing history, the bookmarks and the search engine.
class SuggestionProvider final : public WebView::OmniboxSuggestionProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit SuggestionProvider(WebView::IsPrivate);
    virtual ~SuggestionProvider() override = default;

    virtual void query(WebView::AutocompleteQueryID, String, size_t max_suggestions) override;
    virtual void cancel() override;
    virtual void record_engagement(WebView::OmniboxEngagement) override;

private:
    WebView::Autocomplete m_autocomplete;
};

}
