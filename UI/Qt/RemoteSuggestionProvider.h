/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Omnibox.h>

namespace Ladybird {

// Gets the suggestions of a location bar from the Hub, which has the network and the browsing history they come from.
class RemoteSuggestionProvider final : public WebView::OmniboxSuggestionProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit RemoteSuggestionProvider(WebView::IsPrivate);
    virtual ~RemoteSuggestionProvider() override;

    static RemoteSuggestionProvider* find(u64 client_id);

    virtual void query(WebView::AutocompleteQueryID, String, size_t max_suggestions) override;
    virtual void cancel() override;
    virtual void record_engagement(WebView::OmniboxEngagement) override;

private:
    u64 m_client_id { 0 };
};

}
