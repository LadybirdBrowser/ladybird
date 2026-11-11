/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibURL/URL.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ExternalURLHandler.h>
#include <LibWebView/Forward.h>
#include <LibWebView/OmniboxEngagement.h>
#include <LibWebView/Settings.h>
#include <LibWebView/URL.h>

namespace WebView {

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
    // A remote suggestion that is an address rather than something to search for.
    RemoteURL,
    // Without a search engine, a query that would be searched for offers to choose one instead.
    ChooseSearchEngine,
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
    Optional<ByteBuffer> favicon_png;
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
    Optional<ByteBuffer> favicon_png;
};

// A remote suggestion, classified once when it arrives.
struct RemoteAutocompleteSuggestion {
    String text;
    // The address it goes to, if it is a web address on a public site.
    Optional<String> url;
    // Text without a scheme is an address only if the user has been to this site.
    Optional<String> host_family_the_user_must_have_visited;
};

enum class LocalSuggestionsAreComplete {
    No,
    Yes,
};

WEBVIEW_API String autocomplete_suggestion_display_text(AutocompleteSuggestion const&);
WEBVIEW_API Vector<AutocompleteMatchRange> autocomplete_match_ranges(StringView input, StringView text);
// Remote suggestions for the previous text stay while the user types, as long as they still start with the text.
WEBVIEW_API bool remote_autocomplete_suggestion_matches_input(StringView suggestion, StringView input);
// The rows for remote suggestions. A web address without a scheme is a page only for a site among the local rows. Until
// the local rows are complete, such an address for a site not among them yet is left out, rather than shown as a search
// that turns into a page.
WEBVIEW_API Vector<AutocompleteSuggestion> make_remote_autocomplete_suggestions(Vector<String> remote_suggestions, ReadonlySpan<AutocompleteSuggestion> local_suggestions = {}, LocalSuggestionsAreComplete = LocalSuggestionsAreComplete::Yes);
// The row for the text as typed: the address it is, or else a search for it.
WEBVIEW_API Optional<AutocompleteSuggestion> make_verbatim_autocomplete_suggestion(StringView query, ClassifiedUserInput const&, Optional<SearchEngine> const&);
// For typed text without a scheme that is taken for a web address, such as "setup.py", a search for it as well.
WEBVIEW_API Optional<AutocompleteSuggestion> make_search_suggestion_for_address_like_text(StringView query, ClassifiedUserInput const&, Optional<SearchEngine> const&);
WEBVIEW_API Vector<AutocompleteSuggestion> web_ui_autocomplete_suggestions(StringView input);
WEBVIEW_API size_t autocomplete_preliminary_limit(size_t max_suggestions);
WEBVIEW_API bool autocomplete_url_can_complete(StringView query, StringView suggestion);

// The application that runs the omnibox supplies the things suggestions are made from.
class WEBVIEW_API AutocompleteHost {
public:
    struct RemoteResponse {
        bool had_network_error { false };
        Optional<u32> response_code;
        Optional<ByteString> content_type;
        ByteBuffer body;
    };

    virtual ~AutocompleteHost() = default;

    virtual AutocompleteService& autocomplete_service() = 0;
    virtual Settings& autocomplete_settings() = 0;
    virtual void resolve_external_url_handler(URL::URL const&, ExternalURLHandlerCallback) const = 0;

    // Fetches the engine's suggestions for the query. Returns a function that cancels the request. on_response is never
    // called after that.
    virtual Function<void()> fetch_remote_suggestions(SearchEngine const&, String const& query, IsPrivate, Function<void(RemoteResponse)> on_response) = 0;
};

class WEBVIEW_API Autocomplete : public Weakable<Autocomplete>
    , public SettingsObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    Autocomplete(AutocompleteHost&, IsPrivate);
    virtual ~Autocomplete() override;

    Function<void(AutocompleteQueryID, Vector<AutocompleteSuggestion>, AutocompleteResultKind)> on_autocomplete_query_complete;

    // What the application adds is ranked with the rest, as it does not come from anything known here.
    void query_autocomplete_engine(AutocompleteQueryID, String, size_t max_suggestions = default_autocomplete_suggestion_limit, Vector<AutocompleteSuggestion> additional_suggestions = {});
    void cancel_pending_query();
    void record_engagement(OmniboxEngagement);

private:
    virtual void search_engine_settings_changed() override;

    static ErrorOr<Vector<String>> received_autocomplete_response(SearchSuggestions const&, Optional<ByteString const&> content_type, StringView response);
    void start_remote_query(AutocompleteQueryID, SearchEngine, String query);
    void local_query_complete(AutocompleteQueryID, Vector<AutocompleteSuggestion>);
    void external_url_handler_query_complete(AutocompleteQueryID, RefPtr<ExternalURLHandler>);
    void deliver_current_result();
    void invoke_autocomplete_query_complete(AutocompleteQueryID, Vector<AutocompleteSuggestion> suggestions, AutocompleteResultKind) const;

    AutocompleteHost& m_host;
    IsPrivate m_is_private { IsPrivate::No };
    u64 m_service_client_id { 0 };
    Optional<AutocompleteQueryID> m_query_id;
    String m_query;
    String m_trimmed_query;
    // NB: Classifying the query looks at the file system, so it is classified once, when the query starts.
    ClassifiedUserInput m_classified_query;
    size_t m_max_suggestions { default_autocomplete_suggestion_limit };
    bool m_local_query_complete { false };
    bool m_remote_query_complete { false };
    bool m_external_url_handler_query_complete { true };
    bool m_external_url_has_handler { false };
    Optional<URL::URL> m_external_url;
    Vector<AutocompleteSuggestion> m_local_suggestions;
    // NB: The sites among the local rows, which tell which remote addresses are pages. They are kept for as long as remote
    //     rows carry over to the next text, so a remote page stays one while the local rows for that text load.
    HashTable<String> m_visited_host_families;
    Vector<RemoteAutocompleteSuggestion> m_remote_suggestions;
    RefPtr<Core::Timer> m_remote_query_timer;
    Function<void()> m_cancel_remote_query;
};

}
