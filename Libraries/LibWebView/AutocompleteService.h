/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibDatabase/Forward.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Export.h>

namespace WebView {

// Serializes local omnibox queries on one background thread. The worker owns a separate SQLite
// connection, so neither retrieval nor ranking touches the UI thread's prepared statements.
class WEBVIEW_API AutocompleteService {
    AK_MAKE_NONCOPYABLE(AutocompleteService);
    AK_MAKE_NONMOVABLE(AutocompleteService);

public:
    using ClientID = u64;
    using OnQueryComplete = Function<void(AutocompleteQueryID, Vector<AutocompleteSuggestion>)>;

    AutocompleteService(Core::EventLoop&, Optional<ByteString> history_database_directory);
    ~AutocompleteService();

    ClientID register_client(OnQueryComplete);
    void unregister_client(ClientID);

    void query(ClientID, AutocompleteQueryID, String, size_t max_suggestions);
    void cancel(ClientID);
    void update_bookmarks(Vector<AutocompleteBookmark>);
    void record_engagement(OmniboxEngagement);

private:
    struct Client final : public RefCounted<Client> {
        explicit Client(OnQueryComplete callback)
            : on_query_complete(move(callback))
        {
        }

        OnQueryComplete on_query_complete;
        Optional<AutocompleteQueryID> active_query_id;
    };

    struct Query {
        ClientID client_id { 0 };
        AutocompleteQueryID query_id { 0 };
        String input;
        size_t max_suggestions { 0 };
    };

    intptr_t worker_main(Optional<ByteString> history_database_directory);
    void deliver(Query const&, Vector<AutocompleteSuggestion>);
    bool query_is_current(Query const&);

    Core::EventLoop& m_main_event_loop;
    HashMap<ClientID, NonnullRefPtr<Client>> m_clients;
    ClientID m_next_client_id { 0 };

    Sync::Mutex m_worker_mutex;
    Sync::ConditionVariable m_worker_condition { m_worker_mutex };
    HashMap<ClientID, Query> m_active_queries;
    Vector<Query> m_pending_queries;
    Optional<Vector<AutocompleteBookmark>> m_pending_bookmarks;
    Vector<OmniboxEngagement> m_pending_engagements;
    Optional<ClientID> m_running_query_client_id;
    Database::Database* m_interruptible_database { nullptr };
    bool m_stopping { false };
    NonnullRefPtr<Threading::Thread> m_worker;
};

}
