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
#include <LibCore/Forward.h>
#include <LibRequests/Forward.h>
#include <LibWebView/Forward.h>
#include <LibWebView/OmniboxEngagement.h>
#include <LibWebView/PrivateBrowsing.h>

namespace WebView {

struct AutocompleteEngine {
    StringView name;
    StringView query_url;
};

enum class AutocompleteResultKind {
    Intermediate,
    Final,
};

using AutocompleteQueryID = u64;

static constexpr auto default_autocomplete_suggestion_limit = 8uz;

enum class AutocompleteSuggestionSource {
    LiteralURL,
    WebUI,
    History,
    Bookmark,
    Adaptive,
    Search,
};

enum class AutocompleteMatchClass {
    None,
    ExactURL,
    ExactTitle,
    URLPrefix,
    TitlePrefix,
    URLSubstring,
    TitleSubstring,
    AdaptiveExact,
    AdaptivePrefix,
};

struct WEBVIEW_API AutocompleteSuggestion {
    AutocompleteSuggestionSource source { AutocompleteSuggestionSource::Search };
    String text;
    Optional<String> title;
    Optional<String> subtitle;
    Optional<String> favicon_base64_png;
    String highlight_input;
    AutocompleteMatchClass match_class { AutocompleteMatchClass::None };
    i32 relevance { 0 };
    i32 match_relevance { 0 };
    i32 history_relevance { 0 };
    i32 bookmark_relevance { 0 };
    i32 adaptive_relevance { 0 };
    bool is_verbatim { false };
    bool can_be_automatically_selected { true };
    bool can_be_inline_completed { false };
};

struct AutocompleteMatchRange {
    size_t start { 0 };
    size_t length { 0 };
};

struct WEBVIEW_API AutocompleteBookmark {
    String url;
    Optional<String> title;
    Optional<String> folder;
    Optional<String> favicon_base64_png;
};

WEBVIEW_API ReadonlySpan<AutocompleteEngine> autocomplete_engines();
WEBVIEW_API Optional<AutocompleteEngine const&> find_autocomplete_engine_by_name(StringView name);
WEBVIEW_API String autocomplete_suggestion_display_text(AutocompleteSuggestion const&);
WEBVIEW_API Vector<AutocompleteMatchRange> autocomplete_match_ranges(StringView input, StringView text);
WEBVIEW_API Vector<String> filter_remote_autocomplete_suggestions(StringView input, Vector<String> suggestions);
WEBVIEW_API Vector<AutocompleteSuggestion> web_ui_autocomplete_suggestions(StringView input);
WEBVIEW_API bool autocomplete_urls_match(StringView left, StringView right);
WEBVIEW_API bool autocomplete_url_can_complete(StringView query, StringView suggestion);

class WEBVIEW_API Autocomplete {
public:
    explicit Autocomplete(IsPrivate);
    ~Autocomplete();

    Function<void(AutocompleteQueryID, Vector<AutocompleteSuggestion>, AutocompleteResultKind)> on_autocomplete_query_complete;

    void query_autocomplete_engine(AutocompleteQueryID, String, size_t max_suggestions = default_autocomplete_suggestion_limit);
    void cancel_pending_query();
    void record_engagement(OmniboxEngagement);

private:
    static ErrorOr<Vector<String>> received_autocomplete_respsonse(AutocompleteEngine const&, Optional<ByteString const&> content_type, StringView response);
    void start_remote_query(AutocompleteQueryID, AutocompleteEngine, String query);
    void local_query_complete(AutocompleteQueryID, Vector<AutocompleteSuggestion>);
    void deliver_current_result();
    void invoke_autocomplete_query_complete(AutocompleteQueryID, Vector<AutocompleteSuggestion> suggestions, AutocompleteResultKind) const;

    IsPrivate m_is_private { IsPrivate::No };
    u64 m_service_client_id { 0 };
    Optional<AutocompleteQueryID> m_query_id;
    String m_query;
    String m_trimmed_query;
    size_t m_max_suggestions { default_autocomplete_suggestion_limit };
    bool m_local_query_complete { false };
    bool m_remote_query_complete { false };
    Vector<AutocompleteSuggestion> m_local_suggestions;
    Vector<String> m_remote_suggestions;
    RefPtr<Core::Timer> m_remote_query_timer;
    RefPtr<Requests::Request> m_request;
};

}
