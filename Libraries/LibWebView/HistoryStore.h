/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibDatabase/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

struct WEBVIEW_API HistoryEntry {
    String url;
    Optional<String> title;
    Optional<String> favicon_base64_png;
    u64 visit_count { 0 };
    UnixDateTime last_visited_time;
};

class WEBVIEW_API HistoryStore {
    AK_MAKE_NONCOPYABLE(HistoryStore);
    AK_MAKE_NONMOVABLE(HistoryStore);

public:
    static ErrorOr<NonnullOwnPtr<HistoryStore>> create(Database::Database&);
    static NonnullOwnPtr<HistoryStore> create();
    static NonnullOwnPtr<HistoryStore> create_disabled();
    static Optional<String> normalize_url(URL::URL const&);

    ~HistoryStore();

    void record_visit(URL::URL const&, Optional<String> title = {}, UnixDateTime visited_at = UnixDateTime::now());
    void update_title(URL::URL const&, String const& title);
    void update_favicon(URL::URL const&, String const& favicon_base64_png);

    Optional<HistoryEntry> entry_for_url(URL::URL const&);
    Vector<HistoryEntry> autocomplete_entries(StringView query, size_t limit = 8);

    void clear();
    void remove_entries_accessed_since(UnixDateTime since);

private:
    struct Statements {
        Database::StatementID upsert_entry { 0 };
        Database::StatementID update_title { 0 };
        Database::StatementID update_favicon { 0 };
        Database::StatementID get_entry { 0 };
        Database::StatementID search_entries { 0 };
        Database::StatementID clear_entries { 0 };
        Database::StatementID delete_entries_accessed_since { 0 };
    };

    class TransientStorage {
    public:
        void record_visit(String url, Optional<String> title, UnixDateTime visited_at);
        void update_title(String const& url, String title);
        void update_favicon(String const& url, String favicon_base64_png);

        Optional<HistoryEntry> entry_for_url(String const& url);
        Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit);

        void clear();
        void remove_entries_accessed_since(UnixDateTime since);

    private:
        HashMap<String, HistoryEntry> m_entries;
    };

    struct PersistedStorage {
        void record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at);
        void update_title(String const& url, String const& title);
        void update_favicon(String const& url, String const& favicon_base64_png);

        Optional<HistoryEntry> entry_for_url(String const& url);
        Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit);

        void clear();
        void remove_entries_accessed_since(UnixDateTime since);

        Database::Database& database;
        Statements statements;
    };

    explicit HistoryStore(Optional<PersistedStorage>, bool is_disabled = false);

    Optional<PersistedStorage> m_persisted_storage;
    TransientStorage m_transient_storage;
    bool m_is_disabled { false };
};

}
