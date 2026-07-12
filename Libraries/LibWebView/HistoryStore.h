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
#include <AK/Vector.h>
#include <LibDatabase/Forward.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/HistoryVisitTransition.h>
#include <LibWebView/OmniboxEngagement.h>

namespace WebView {

struct WEBVIEW_API HistoryEntry {
    String url;
    Optional<String> title;
    Optional<String> favicon_base64_png;
    u64 visit_count { 0 };
    u64 direct_visit_count { 0 };
    UnixDateTime last_visited_time;
    UnixDateTime last_qualifying_visit_time;
    UnixDateTime last_direct_visit_time;
    double decayed_visit_score { 0 };
    double decayed_direct_score { 0 };
    UnixDateTime score_updated_at;
};

struct WEBVIEW_API RecentlyClosedEntry {
    Vector<URL::URL> urls;
    bool was_window { false };
    size_t active_tab_index { 0 };
    UnixDateTime closed_time;
};

enum class RemoveHistoryEntryEngagements {
    No,
    Yes,
};

class WEBVIEW_API HistoryStore {
    AK_MAKE_NONCOPYABLE(HistoryStore);
    AK_MAKE_NONMOVABLE(HistoryStore);

public:
    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);

    static ErrorOr<NonnullOwnPtr<HistoryStore>> create(Database::Database&);
    static NonnullOwnPtr<HistoryStore> create();
    static NonnullOwnPtr<HistoryStore> create_disabled();
    static Optional<String> normalize_url(URL::URL const&);

    ~HistoryStore();

    void record_visit(URL::URL const&, Optional<String> title = {}, UnixDateTime visited_at = UnixDateTime::now(), HistoryVisitTransition = HistoryVisitTransition::Link);
    void update_title(URL::URL const&, String const& title);
    void update_favicon(URL::URL const&, String const& favicon_base64_png);

    void record_closed_tab(URL::URL const&, UnixDateTime closed_at = UnixDateTime::now());
    void record_closed_window(Vector<URL::URL>, size_t active_tab_index, UnixDateTime closed_at = UnixDateTime::now());
    bool has_recently_closed_entries() const;
    Optional<RecentlyClosedEntry const&> most_recently_closed_entry() const;
    Optional<RecentlyClosedEntry> pop_most_recently_closed_entry();

    Optional<HistoryEntry> entry_for_url(URL::URL const&);
    Vector<HistoryEntry> autocomplete_entries(StringView query, size_t limit = 8);
    Vector<HistoryEntry> list_entries(StringView query = {}, size_t offset = 0, size_t limit = 50);

    void record_omnibox_engagement(OmniboxEngagement const&, UnixDateTime used_at = UnixDateTime::now());
    Vector<StoredOmniboxEngagement> omnibox_engagements(StringView input, size_t limit = 50);

    void remove_entry_for_url(URL::URL const&, RemoveHistoryEntryEngagements = RemoveHistoryEntryEngagements::Yes);
    void remove_entries_for_same_site(URL::URL const&);
    void remove_entries_accessed_since(UnixDateTime since);

private:
    struct Statements {
        Database::StatementID upsert_entry { 0 };
        Database::StatementID update_title { 0 };
        Database::StatementID update_favicon { 0 };
        Database::StatementID get_entry { 0 };
        Database::StatementID search_entries { 0 };
        Database::StatementID list_entries { 0 };
        Database::StatementID delete_entry { 0 };
        Database::StatementID delete_entries_accessed_since { 0 };
        Database::StatementID all_urls { 0 };
        Database::StatementID upsert_omnibox_engagement { 0 };
        Database::StatementID search_omnibox_engagements { 0 };
        Database::StatementID delete_omnibox_engagements_for_url { 0 };
        Database::StatementID delete_omnibox_engagements_used_since { 0 };
    };

    class StorageImpl {
    public:
        virtual ~StorageImpl() = default;

        virtual StringView name() = 0;

        virtual void record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at, HistoryVisitTransition) = 0;
        virtual void update_title(String const& url, String const& title) = 0;
        virtual void update_favicon(String const& url, String const& favicon_base64_png) = 0;

        virtual Optional<HistoryEntry> entry_for_url(String const& url) = 0;
        virtual Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit) = 0;
        virtual Vector<HistoryEntry> list_entries(StringView title_query, StringView url_query, size_t offset, size_t limit) = 0;

        virtual void record_omnibox_engagement(OmniboxEngagement const&, UnixDateTime used_at) = 0;
        virtual Vector<StoredOmniboxEngagement> omnibox_engagements(StringView normalized_url_input, StringView normalized_search_input, size_t limit) = 0;

        virtual void remove_entry_for_url(String const& url, RemoveHistoryEntryEngagements) = 0;
        virtual void remove_entries_for_same_site(StringView site_key) = 0;
        virtual void remove_entries_accessed_since(UnixDateTime since) = 0;
    };

    class TransientStorage : public StorageImpl {
    public:
        virtual ~TransientStorage() override = default;

        virtual StringView name() override { return "transient"sv; }

        virtual void record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at, HistoryVisitTransition) override;
        virtual void update_title(String const& url, String const& title) override;
        virtual void update_favicon(String const& url, String const& favicon_base64_png) override;

        virtual Optional<HistoryEntry> entry_for_url(String const& url) override;
        virtual Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit) override;
        virtual Vector<HistoryEntry> list_entries(StringView title_query, StringView url_query, size_t offset, size_t limit) override;

        virtual void record_omnibox_engagement(OmniboxEngagement const&, UnixDateTime used_at) override;
        virtual Vector<StoredOmniboxEngagement> omnibox_engagements(StringView normalized_url_input, StringView normalized_search_input, size_t limit) override;

        virtual void remove_entry_for_url(String const& url, RemoveHistoryEntryEngagements) override;
        virtual void remove_entries_for_same_site(StringView site_key) override;
        virtual void remove_entries_accessed_since(UnixDateTime since) override;

    private:
        HashMap<String, HistoryEntry> m_entries;
        Vector<StoredOmniboxEngagement> m_omnibox_engagements;
    };

    class PersistedStorage : public StorageImpl {
    public:
        PersistedStorage(Database::Database&, Statements&&);
        virtual ~PersistedStorage() override;

        virtual StringView name() override { return "SQL"sv; }

        virtual void record_visit(String const& url, Optional<String> const& title, UnixDateTime visited_at, HistoryVisitTransition) override;
        virtual void update_title(String const& url, String const& title) override;
        virtual void update_favicon(String const& url, String const& favicon_base64_png) override;

        virtual Optional<HistoryEntry> entry_for_url(String const& url) override;
        virtual Vector<HistoryEntry> autocomplete_entries(StringView title_query, StringView url_query, size_t limit) override;
        virtual Vector<HistoryEntry> list_entries(StringView title_query, StringView url_query, size_t offset, size_t limit) override;

        virtual void record_omnibox_engagement(OmniboxEngagement const&, UnixDateTime used_at) override;
        virtual Vector<StoredOmniboxEngagement> omnibox_engagements(StringView normalized_url_input, StringView normalized_search_input, size_t limit) override;

        virtual void remove_entry_for_url(String const& url, RemoveHistoryEntryEngagements) override;
        virtual void remove_entries_for_same_site(StringView site_key) override;
        virtual void remove_entries_accessed_since(UnixDateTime since) override;

    private:
        Database::Database& m_database;
        Statements m_statements;
    };

    explicit HistoryStore(NonnullOwnPtr<StorageImpl>&&, bool is_disabled = false);

    NonnullOwnPtr<StorageImpl> m_storage;
    Vector<RecentlyClosedEntry> m_recently_closed_entries;
    bool m_is_disabled { false };
};

}
