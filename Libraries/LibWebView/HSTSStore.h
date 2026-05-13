/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/Timer.h>
#include <LibDatabase/Forward.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API HSTSStore {
public:
    static ErrorOr<NonnullOwnPtr<HSTSStore>> create(Database::Database&);
    static NonnullOwnPtr<HSTSStore> create();

    ~HSTSStore();

    // https://www.rfc-editor.org/rfc/rfc6797#section-8.1
    void store_policy(String const& domain, HTTP::HSTS::ParsedHSTSPolicy const& policy);

    // https://www.rfc-editor.org/rfc/rfc6797#section-8.2
    bool is_known_hsts_host(StringView domain);

    void remove_policies_observed_since(UnixDateTime since);

private:
    struct StoredPolicy {
        UnixDateTime expiry;
        bool include_sub_domains { false };
        UnixDateTime last_observed_time;
    };

    struct Statements {
        Database::StatementID insert_policy { 0 };
        Database::StatementID delete_expired { 0 };
        Database::StatementID select_all_policies { 0 };
    };

    class WEBVIEW_API TransientStorage {
    public:
        using Policies = HashMap<String, StoredPolicy>;

        void set_policies(Policies);
        void set_policy(String const& domain, StoredPolicy const& policy);
        Optional<StoredPolicy const&> get_policy(StringView domain) const;
        void update_last_observed_time(StringView domain);

        UnixDateTime purge_expired_policies();
        void remove_policies_observed_since(UnixDateTime since);

        auto take_dirty_policies() { return move(m_dirty_policies); }

    private:
        Policies m_policies;
        Policies m_dirty_policies;
    };

    struct WEBVIEW_API PersistedStorage {
        void insert_policy(String const& domain, StoredPolicy const& policy);
        TransientStorage::Policies select_all_policies();

        Database::Database& database;
        Statements statements;
        RefPtr<Core::Timer> synchronization_timer {};
    };

    explicit HSTSStore(Optional<PersistedStorage>);

    AK_MAKE_NONCOPYABLE(HSTSStore);
    AK_MAKE_NONMOVABLE(HSTSStore);

    Optional<PersistedStorage> m_persisted_storage;
    TransientStorage m_transient_storage;
};

}
