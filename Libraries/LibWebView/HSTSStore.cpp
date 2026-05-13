/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibDatabase/Database.h>
#include <LibWebView/HSTSStore.h>

namespace WebView {

static constexpr auto DATABASE_SYNCHRONIZATION_TIMER = AK::Duration::from_seconds(30);

ErrorOr<NonnullOwnPtr<HSTSStore>> HSTSStore::create(Database::Database& database)
{
    Statements statements {};

    auto create_table = TRY(database.prepare_statement("CREATE TABLE IF NOT EXISTS HSTSPolicies ("
                                                       "    domain TEXT PRIMARY KEY,"
                                                       "    expiry_time INTEGER NOT NULL,"
                                                       "    include_sub_domains BOOLEAN NOT NULL,"
                                                       "    last_observed_time INTEGER NOT NULL"
                                                       ");"sv));
    database.execute_statement(create_table, {});

    statements.insert_policy = TRY(database.prepare_statement("INSERT OR REPLACE INTO HSTSPolicies VALUES (?, ?, ?, ?);"sv));
    statements.delete_expired = TRY(database.prepare_statement("DELETE FROM HSTSPolicies WHERE (expiry_time < ?);"sv));
    statements.select_all_policies = TRY(database.prepare_statement("SELECT * FROM HSTSPolicies;"sv));

    return adopt_own(*new HSTSStore { PersistedStorage { database, statements } });
}

NonnullOwnPtr<HSTSStore> HSTSStore::create()
{
    return adopt_own(*new HSTSStore { OptionalNone {} });
}

HSTSStore::HSTSStore(Optional<PersistedStorage> persisted_storage)
    : m_persisted_storage(move(persisted_storage))
{
    if (!m_persisted_storage.has_value())
        return;

    auto policies = m_persisted_storage->select_all_policies();
    m_transient_storage.set_policies(move(policies));

    m_persisted_storage->synchronization_timer = Core::Timer::create_repeating(
        static_cast<int>(DATABASE_SYNCHRONIZATION_TIMER.to_milliseconds()),
        [this]() {
            for (auto const& it : m_transient_storage.take_dirty_policies())
                m_persisted_storage->insert_policy(it.key, it.value);

            auto now = m_transient_storage.purge_expired_policies();
            m_persisted_storage->database.execute_statement(m_persisted_storage->statements.delete_expired, {}, now.milliseconds_since_epoch());
        });
    m_persisted_storage->synchronization_timer->start();
}

HSTSStore::~HSTSStore()
{
    if (!m_persisted_storage.has_value())
        return;

    m_persisted_storage->synchronization_timer->stop();
    m_persisted_storage->synchronization_timer->on_timeout();
}

// https://www.rfc-editor.org/rfc/rfc6797#section-8.1
void HSTSStore::store_policy(String const& domain, HTTP::HSTS::ParsedHSTSPolicy const& policy)
{
    // NB: The caller is responsible for ensuring this is only called for responses received over
    //     secure transport, and that the host is a domain (not an IP address).

    auto now = UnixDateTime::now();
    StoredPolicy stored_policy;

    // A max-age value of zero (i.e., "max-age=0") signals the UA to cease regarding the host as a Known HSTS Host,
    // including the includeSubDomains directive (if asserted for that HSTS Host).
    if (policy.max_age == AK::Duration::zero()) {
        stored_policy = StoredPolicy {
            .expiry = UnixDateTime::earliest(),
            .include_sub_domains = false,
            .last_observed_time = now,
        };
    } else {
        stored_policy = StoredPolicy {
            .expiry = now + policy.max_age,
            .include_sub_domains = policy.include_sub_domains,
            .last_observed_time = now,
        };
    }

    m_transient_storage.set_policy(domain.to_ascii_lowercase(), stored_policy);
    m_transient_storage.purge_expired_policies();
}

// https://www.rfc-editor.org/rfc/rfc6797#section-8.2
bool HSTSStore::is_known_hsts_host(StringView domain)
{
    m_transient_storage.purge_expired_policies();

    // Compare the given domain name with the domain name of each of the UA's unexpired Known HSTS Hosts.
    // For each Known HSTS Host's domain name, the comparison is done with the given domain name label-by-label
    // (comparing only labels) using an ASCII case-insensitive comparison beginning with the rightmost label,
    // and continuing right-to-left.
    auto canonical = domain.to_ascii_lowercase_string();

    // Congruent Match: If a label-for-label match between a Known HSTS Host's domain name and the given domain name
    // is found -- i.e., there are no further labels to compare -- then the given domain name congruently matches
    // this Known HSTS Host.
    if (auto policy = m_transient_storage.get_policy(canonical); policy.has_value()) {
        m_transient_storage.update_last_observed_time(canonical);
        return true;
    }

    // Superdomain Match: If a label-for-label match between an entire Known HSTS Host's domain name and a right-hand
    // portion of the given domain name is found, then this Known HSTS Host's domain name is a superdomain match for
    // the given domain name. There could be multiple superdomain matches for a given domain name.
    auto remaining = canonical.bytes_as_string_view();
    while (true) {
        auto dot = remaining.find('.');
        if (!dot.has_value())
            break;
        remaining = remaining.substring_view(*dot + 1);
        if (auto policy = m_transient_storage.get_policy(remaining); policy.has_value() && policy->include_sub_domains) {
            m_transient_storage.update_last_observed_time(remaining);
            return true;
        }
    }

    // Otherwise, if no matches are found, the given domain name does not represent a Known HSTS Host.
    return false;
}

void HSTSStore::remove_policies_observed_since(UnixDateTime since)
{
    m_transient_storage.remove_policies_observed_since(since);
}

void HSTSStore::TransientStorage::set_policies(Policies policies)
{
    m_policies = move(policies);
    purge_expired_policies();
}

void HSTSStore::TransientStorage::set_policy(String const& domain, StoredPolicy const& policy)
{
    auto now = UnixDateTime::now();
    if (policy.expiry < now && !m_policies.contains(domain))
        return;

    m_policies.set(domain, policy);
    m_dirty_policies.set(domain, policy);
}

Optional<HSTSStore::StoredPolicy const&> HSTSStore::TransientStorage::get_policy(StringView domain) const
{
    auto it = m_policies.find(domain);
    if (it == m_policies.end())
        return {};
    return it->value;
}

void HSTSStore::TransientStorage::update_last_observed_time(StringView domain)
{
    auto it = m_policies.find(domain);
    if (it == m_policies.end())
        return;

    it->value.last_observed_time = UnixDateTime::now();
    m_dirty_policies.set(it->key, it->value);
}

void HSTSStore::TransientStorage::remove_policies_observed_since(UnixDateTime since)
{
    for (auto& [domain, policy] : m_policies) {
        if (policy.last_observed_time >= since) {
            policy.expiry = UnixDateTime::earliest();
            m_dirty_policies.set(domain, policy);
        }
    }

    purge_expired_policies();
}

UnixDateTime HSTSStore::TransientStorage::purge_expired_policies()
{
    // A Known HSTS Host is "expired" if its cache entry has an expiry date
    // in the past. The UA MUST evict all expired Known HSTS Hosts from its
    // cache if, at any time, an expired Known HSTS Host exists in the
    // cache.
    auto now = UnixDateTime::now();
    auto is_expired = [&](auto const&, auto const& policy) { return policy.expiry < now; };
    m_policies.remove_all_matching(is_expired);
    return now;
}

void HSTSStore::PersistedStorage::insert_policy(String const& domain, StoredPolicy const& policy)
{
    database.execute_statement(statements.insert_policy, {}, domain, policy.expiry, policy.include_sub_domains, policy.last_observed_time);
}

HSTSStore::TransientStorage::Policies HSTSStore::PersistedStorage::select_all_policies()
{
    TransientStorage::Policies policies;

    database.execute_statement(statements.select_all_policies, [&](auto row) {
        auto domain = database.result_column<String>(row, 0);
        StoredPolicy stored_policy {
            .expiry = database.result_column<UnixDateTime>(row, 1),
            .include_sub_domains = database.result_column<bool>(row, 2),
            .last_observed_time = database.result_column<UnixDateTime>(row, 3),
        };
        policies.set(move(domain), stored_policy);
    });

    return policies;
}

}
