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

    class StorageImpl {
    public:
        virtual ~StorageImpl() = default;

        virtual StringView name() = 0;

        virtual void record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at) = 0;
        virtual void update_title(String const& url, String const& title) = 0;
        virtual void update_favicon(String const& url, String const& favicon_base64_png) = 0;

        virtual Optional<HistoryEntry> entry_for_url(String const& url) = 0;
        virtual Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit) = 0;

        virtual void clear() = 0;
        virtual void remove_entries_accessed_since(UnixDateTime since) = 0;
    };

    class TransientStorage : public StorageImpl {
    public:
        virtual ~TransientStorage() override = default;

        virtual StringView name() override { return "transient"sv; }

        virtual void record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at) override;
        virtual void update_title(String const& url, String const& title) override;
        virtual void update_favicon(String const& url, String const& favicon_base64_png) override;

        virtual Optional<HistoryEntry> entry_for_url(String const& url) override;
        virtual Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit) override;

        virtual void clear() override;
        virtual void remove_entries_accessed_since(UnixDateTime since) override;

    private:
        HashMap<String, HistoryEntry> m_entries;
    };

    class PersistedStorage : public StorageImpl {
    public:
        PersistedStorage(Database::Database&, Statements&&);
        virtual ~PersistedStorage() override;

        virtual StringView name() override { return "SQL"sv; }

        virtual void record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at) override;
        virtual void update_title(String const& url, String const& title) override;
        virtual void update_favicon(String const& url, String const& favicon_base64_png) override;

        virtual Optional<HistoryEntry> entry_for_url(String const& url) override;
        virtual Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit) override;

        virtual void clear() override;
        virtual void remove_entries_accessed_since(UnixDateTime since) override;

    private:
        Database::Database& m_database;
        Statements m_statements;
    };

    explicit HistoryStore(NonnullOwnPtr<StorageImpl>&&, bool is_disabled = false);

    NonnullOwnPtr<StorageImpl> m_storage;
    bool m_is_disabled { false };
};

}
