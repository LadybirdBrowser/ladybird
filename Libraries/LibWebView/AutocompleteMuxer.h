/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Export.h>

namespace WebView {

// The host of a URL without "www.", which the pages of a site share.
WEBVIEW_API Optional<String> autocomplete_host_family(URL::URL const&);
// The first of the suggestions that goes where the needle goes.
WEBVIEW_API Optional<size_t> index_of_suggestion_with_same_destination(AutocompleteSuggestion const& needle, ReadonlySpan<AutocompleteSuggestion>);

// Rows for pages of the search engine's results, for search terms that the query starts a word of, become the searches
// for those terms.
WEBVIEW_API void show_search_results_pages_as_searches(StringView query, Vector<AutocompleteSuggestion>&, Optional<SearchEngine> const&);

WEBVIEW_API Vector<AutocompleteSuggestion> mux_autocomplete_suggestions(
    StringView query,
    Optional<AutocompleteSuggestion> verbatim_suggestion,
    Vector<AutocompleteSuggestion> local_suggestions,
    Vector<AutocompleteSuggestion> remote_suggestions,
    size_t limit);

}
