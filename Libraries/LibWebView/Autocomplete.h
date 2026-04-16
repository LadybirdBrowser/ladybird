/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibRequests/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct AutocompleteEngine {
    StringView name;
    StringView query_url;
};

enum class AutocompleteResultKind {
    Intermediate,
    Final,
};

static constexpr auto default_autocomplete_suggestion_limit = 8uz;

enum class AutocompleteSuggestionSource {
    LiteralURL,
    History,
    Search,
};

enum class AutocompleteSuggestionSection {
    None,
    History,
    SearchSuggestions,
};

struct WEBVIEW_API AutocompleteSuggestion {
    AutocompleteSuggestionSource source { AutocompleteSuggestionSource::Search };
    AutocompleteSuggestionSection section { AutocompleteSuggestionSection::None };
    String text;
    Optional<String> title;
    Optional<String> subtitle;
    Optional<String> favicon_base64_png;
};

WEBVIEW_API ReadonlySpan<AutocompleteEngine> autocomplete_engines();
WEBVIEW_API Optional<AutocompleteEngine const&> find_autocomplete_engine_by_name(StringView name);
WEBVIEW_API StringView autocomplete_section_title(AutocompleteSuggestionSection);
WEBVIEW_API bool autocomplete_urls_match(StringView left, StringView right);
WEBVIEW_API bool autocomplete_url_can_complete(StringView query, StringView suggestion);

class WEBVIEW_API Autocomplete {
public:
    Autocomplete();
    ~Autocomplete();

    Function<void(Vector<AutocompleteSuggestion>, AutocompleteResultKind)> on_autocomplete_query_complete;

    void query_autocomplete_engine(String, size_t max_suggestions = default_autocomplete_suggestion_limit);
    void cancel_pending_query();

private:
    static ErrorOr<Vector<String>> received_autocomplete_respsonse(AutocompleteEngine const&, Optional<ByteString const&> content_type, StringView response);
    void invoke_autocomplete_query_complete(Vector<AutocompleteSuggestion> suggestions, AutocompleteResultKind) const;

    String m_query;
    size_t m_max_suggestions { default_autocomplete_suggestion_limit };
    Vector<AutocompleteSuggestion> m_history_suggestions;
    RefPtr<Requests::Request> m_request;
};

}
