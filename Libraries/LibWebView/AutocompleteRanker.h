/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Export.h>
#include <LibWebView/HistoryStore.h>

namespace WebView {

WEBVIEW_API Vector<AutocompleteSuggestion> rank_history_suggestions(StringView query, Vector<HistoryEntry>, size_t limit, UnixDateTime now = UnixDateTime::now());
WEBVIEW_API Vector<AutocompleteSuggestion> rank_bookmark_suggestions(StringView query, Vector<AutocompleteBookmark> const&, size_t limit);
WEBVIEW_API Vector<AutocompleteSuggestion> rank_engagement_suggestions(StringView query, Vector<StoredOmniboxEngagement>, size_t limit, UnixDateTime now = UnixDateTime::now());

}
