/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibDatabase/Database.h>
#include <LibSync/Mutex.h>
#include <LibWebView/AutocompleteRanker.h>
#include <LibWebView/AutocompleteService.h>
#include <LibWebView/HistoryStore.h>

namespace WebView {

AutocompleteService::AutocompleteService(Core::EventLoop& main_event_loop, Optional<ByteString> history_database_directory)
    : m_main_event_loop(main_event_loop)
    , m_worker(Threading::Thread::construct("Omnibox"sv, [this, history_database_directory = move(history_database_directory)]() mutable {
        return worker_main(move(history_database_directory));
    }))
{
    m_worker->start();
}

AutocompleteService::~AutocompleteService()
{
    {
        Sync::MutexLocker locker(m_worker_mutex);
        m_stopping = true;
        m_pending_queries.clear();
        if (m_interruptible_database)
            m_interruptible_database->interrupt();
        m_worker_condition.signal();
    }

    [[maybe_unused]] auto result = m_worker->join();
}

AutocompleteService::ClientID AutocompleteService::register_client(OnQueryComplete on_query_complete)
{
    ++m_next_client_id;
    VERIFY(m_next_client_id != 0);
    m_clients.set(m_next_client_id, adopt_ref(*new Client { move(on_query_complete) }));
    return m_next_client_id;
}

void AutocompleteService::unregister_client(ClientID client_id)
{
    cancel(client_id);
    m_clients.remove(client_id);
}

void AutocompleteService::query(ClientID client_id, AutocompleteQueryID query_id, String input, size_t max_suggestions)
{
    auto client = m_clients.find(client_id);
    if (client == m_clients.end())
        return;
    client->value->active_query_id = query_id;

    Query query { client_id, query_id, move(input), max_suggestions };
    Sync::MutexLocker locker(m_worker_mutex);
    m_active_queries.set(client_id, query);
    m_pending_queries.remove_all_matching([&](auto const& query) {
        return query.client_id == client_id;
    });
    m_pending_queries.append(move(query));
    if (m_running_query_client_id == client_id && m_interruptible_database)
        m_interruptible_database->interrupt();
    m_worker_condition.signal();
}

void AutocompleteService::cancel(ClientID client_id)
{
    if (auto client = m_clients.find(client_id); client != m_clients.end())
        client->value->active_query_id = {};

    Sync::MutexLocker locker(m_worker_mutex);
    m_active_queries.remove(client_id);
    m_pending_queries.remove_all_matching([&](auto const& query) {
        return query.client_id == client_id;
    });
    if (m_running_query_client_id == client_id && m_interruptible_database)
        m_interruptible_database->interrupt();
}

void AutocompleteService::update_bookmarks(Vector<AutocompleteBookmark> bookmarks)
{
    Sync::MutexLocker locker(m_worker_mutex);
    m_pending_bookmarks = move(bookmarks);

    for (auto const& active_query : m_active_queries) {
        m_pending_queries.remove_all_matching([&](auto const& query) {
            return query.client_id == active_query.key;
        });
        m_pending_queries.append(active_query.value);
    }
    m_worker_condition.signal();
}

void AutocompleteService::record_engagement(OmniboxEngagement engagement)
{
    Sync::MutexLocker locker(m_worker_mutex);
    m_pending_engagements.append(move(engagement));
    m_worker_condition.signal();
}

bool AutocompleteService::query_is_current(Query const& query)
{
    Sync::MutexLocker locker(m_worker_mutex);
    if (m_stopping)
        return false;
    auto active_query = m_active_queries.find(query.client_id);
    return active_query != m_active_queries.end()
        && active_query->value.query_id == query.query_id;
}

intptr_t AutocompleteService::worker_main(Optional<ByteString> history_database_directory)
{
    RefPtr<Database::Database> database;
    OwnPtr<HistoryStore> history_store;

    if (history_database_directory.has_value()) {
        auto database_or_error = Database::Database::create(*history_database_directory, "History"sv);
        if (database_or_error.is_error()) {
            warnln("Unable to open the omnibox history database: {}", database_or_error.error());
        } else {
            database = database_or_error.release_value();
            auto history_store_or_error = HistoryStore::create(*database);
            if (history_store_or_error.is_error())
                warnln("Unable to prepare the omnibox history database: {}", history_store_or_error.error());
            else
                history_store = history_store_or_error.release_value();
        }
    }
    if (!history_store)
        history_store = HistoryStore::create();

    Vector<AutocompleteBookmark> bookmarks;

    while (true) {
        Optional<Query> query;
        Optional<Vector<AutocompleteBookmark>> bookmark_update;
        Vector<OmniboxEngagement> engagements;
        {
            Sync::MutexLocker locker(m_worker_mutex);
            m_worker_condition.wait_while([&] {
                return !m_stopping && m_pending_queries.is_empty() && !m_pending_bookmarks.has_value() && m_pending_engagements.is_empty();
            });
            if (m_stopping && m_pending_engagements.is_empty())
                return 0;
            if (!m_stopping && m_pending_bookmarks.has_value())
                bookmark_update = m_pending_bookmarks.release_value();
            if (!m_stopping && !m_pending_queries.is_empty())
                query = m_pending_queries.take_first();
            engagements = move(m_pending_engagements);
        }

        if (bookmark_update.has_value())
            bookmarks = bookmark_update.release_value();
        for (auto const& engagement : engagements)
            history_store->record_omnibox_engagement(engagement, engagement.used_at);
        if (!query.has_value())
            continue;

        {
            Sync::MutexLocker locker(m_worker_mutex);
            m_running_query_client_id = query->client_id;
            m_interruptible_database = database.ptr();
        }
        ScopeGuard clear_running_query = [&] {
            Sync::MutexLocker locker(m_worker_mutex);
            m_running_query_client_id = {};
            m_interruptible_database = nullptr;
        };

        if (!query_is_current(*query))
            continue;

        auto preliminary_limit = max(query->max_suggestions * 4, 32uz);
        auto history_entries = history_store->autocomplete_entries(query->input, preliminary_limit);
        if (!query_is_current(*query))
            continue;
        auto suggestions = rank_history_suggestions(query->input, move(history_entries), preliminary_limit);
        if (!query_is_current(*query))
            continue;
        suggestions.extend(rank_bookmark_suggestions(query->input, bookmarks, preliminary_limit));
        if (!query_is_current(*query))
            continue;
        auto engagements_for_query = history_store->omnibox_engagements(query->input, 200);
        if (!query_is_current(*query))
            continue;
        suggestions.extend(rank_engagement_suggestions(query->input, move(engagements_for_query), preliminary_limit));
        if (!query_is_current(*query))
            continue;
        deliver(*query, move(suggestions));
    }
}

void AutocompleteService::deliver(Query const& query, Vector<AutocompleteSuggestion> suggestions)
{
    m_main_event_loop.deferred_invoke([this, client_id = query.client_id, query_id = query.query_id, suggestions = move(suggestions)]() mutable {
        auto client = m_clients.find(client_id);
        if (client == m_clients.end() || client->value->active_query_id != query_id)
            return;
        auto protected_client = client->value;
        protected_client->on_query_complete(query_id, move(suggestions));
    });
}

}
