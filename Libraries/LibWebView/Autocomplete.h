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
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibRequests/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct AutocompleteEngine {
    StringView name;
    StringView query_url;
};

enum class SuggestionKind {
    Navigational,
    QueryCompletion,
};

enum class SuggestionSource {
    History,
    Bookmark,
    Remote,
};

struct AutocompleteSuggestion {
    String text;
    Optional<String> title;
    SuggestionKind kind { SuggestionKind::QueryCompletion };
    SuggestionSource source { SuggestionSource::History };
    double score { 0 };
};

struct SuggestionOptions {
    bool remote_enabled { true };
    size_t max_results { 8 };
};

struct LocalSuggestionSources {
    Vector<String> bookmarks;
    // Must be ordered newest-to-oldest.
    Vector<String> history_newest_first;
};

struct LocalSuggestionIndexStats {
    size_t total_entries { 0 };
    size_t navigational_entries { 0 };
    size_t query_completion_entries { 0 };
    size_t bookmark_entries { 0 };
    size_t history_entries { 0 };
    size_t unique_tokens { 0 };
    size_t phrase_prefixes { 0 };
    size_t token_prefixes { 0 };
    size_t term_transition_contexts { 0 };
    size_t term_transition_edges { 0 };
    bool is_loaded { false };
    bool is_loading { false };
    bool rebuild_pending { false };
    bool rebuild_in_progress { false };
};

WEBVIEW_API ReadonlySpan<AutocompleteEngine> autocomplete_engines();
WEBVIEW_API Optional<AutocompleteEngine const&> find_autocomplete_engine_by_name(StringView name);

class WEBVIEW_API Autocomplete {
public:
    Autocomplete();
    ~Autocomplete();

    Function<void(Vector<AutocompleteSuggestion>)> on_suggestions_query_complete;
    // FIXME: Remove this callback once all UI integrations consume structured suggestions.
    Function<void(Vector<String>)> on_autocomplete_query_complete;

    void query_suggestions(String, SuggestionOptions = {});
    void query_autocomplete_engine(String);
    void notify_omnibox_interaction();
    void record_committed_input(String const&);
    void record_navigation(String const&, Optional<String> title = {});
    void update_navigation_title(String const&, String const&);
    void record_bookmark(String const&);

    static StringView local_index_rebuild_placeholder_text();

    static void rebuild_local_index_from_sources(LocalSuggestionSources);
    static void rebuild_local_index_from_current_entries();
    static void schedule_local_index_rebuild_after_source_removal();
    static void schedule_local_index_rebuild_after_source_removal(LocalSuggestionSources);
    static LocalSuggestionSources local_index_sources_after_history_deletion(i64 delete_history_since_unix_seconds);
    static void clear_local_index();
    static void flush_local_index_to_disk(Core::EventLoop&);
    static LocalSuggestionIndexStats local_index_stats();

private:
    static ErrorOr<Vector<String>> received_autocomplete_response(AutocompleteEngine const&, Optional<ByteString const&> content_type, StringView response);
    static Vector<AutocompleteSuggestion> merge_suggestions(StringView query, bool prefer_navigational, size_t max_results, Vector<AutocompleteSuggestion> local, Vector<String> remote);
    static void notify_instances_about_local_index_state_change();
    void invoke_suggestions_query_complete(Vector<AutocompleteSuggestion> suggestions) const;
    void refresh_last_query_after_local_index_state_change();

    u64 m_query_sequence_number { 0 };
    String m_query;
    SuggestionOptions m_last_query_options;
    bool m_has_active_query { false };
    bool m_showing_local_index_rebuild_placeholder { false };
    Autocomplete* m_previous_live_instance { nullptr };
    Autocomplete* m_next_live_instance { nullptr };
    RefPtr<Requests::Request> m_request;
};

}
