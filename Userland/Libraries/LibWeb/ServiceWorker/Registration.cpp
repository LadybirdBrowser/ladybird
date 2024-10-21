/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibWeb/ServiceWorker/Registration.h>

namespace Web::ServiceWorker {

struct RegistrationKey {
    StorageAPI::StorageKey key;
    ByteString serialized_scope_url;

    bool operator==(RegistrationKey const&) const = default;
};

// FIXME: Surely this needs hooks to be cleared and manipulated at the UA level
//        Does this need to be serialized to disk as well?
static HashMap<RegistrationKey, Registration> s_registrations;

Registration::Registration(StorageAPI::StorageKey storage_key, URL::URL scope, Bindings::ServiceWorkerUpdateViaCache update_via_cache)
    : m_storage_key(move(storage_key))
    , m_scope_url(move(scope))
    , m_update_via_cache_mode(update_via_cache)
{
}

// https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration-unregistered
bool Registration::is_unregistered()
{
    // A service worker registration is said to be unregistered if registration map[this service worker registration's (storage key, serialized scope url)] is not this service worker registration.
    // FIXME: Suspect that spec should say to serialize without fragment
    auto const key = RegistrationKey { m_storage_key, m_scope_url.serialize(URL::ExcludeFragment::Yes) };
    return s_registrations.get(key).map([](auto& registration) { return &registration; }).value_or(nullptr) != this;
}

// https://w3c.github.io/ServiceWorker/#service-worker-registration-stale
bool Registration::is_stale() const
{
    using namespace AK::TimeLiterals;

    // A service worker registration is said to be stale if the registration’s last update check time is non-null
    // and the time difference in seconds calculated by the current time minus the registration’s last update check time is greater than 86400.
    if (!m_last_update_check_time.has_value())
        return false;

    return (MonotonicTime::now() - m_last_update_check_time.value()) > 86400_sec;
}

// https://w3c.github.io/ServiceWorker/#get-registration-algorithm
Optional<Registration&> Registration::get(StorageAPI::StorageKey const& key, Optional<URL::URL> scope)
{
    // 1. Run the following steps atomically.
    // FIXME: What does this mean? Do we need a mutex? does it need to be 'locked' at the UA level?

    // 2. Let scopeString be the empty string.
    ByteString scope_string;

    // 3. If scope is not null, set scopeString to serialized scope with the exclude fragment flag set.
    if (scope.has_value())
        scope_string = scope.value().serialize(URL::ExcludeFragment::Yes);

    // 4. For each (entry storage key, entry scope) → registration of registration map:
    //   1. If storage key equals entry storage key and scopeString matches entry scope, then return registration.
    // 5. Return null.
    return s_registrations.get({ key, scope_string });
}

// https://w3c.github.io/ServiceWorker/#set-registration-algorithm
Registration& Registration::set(StorageAPI::StorageKey const& storage_key, URL::URL const& scope, Bindings::ServiceWorkerUpdateViaCache update_via_cache)
{
    // FIXME: 1. Run the following steps atomically.

    // 2. Let scopeString be serialized scope with the exclude fragment flag set.
    // 3. Let registration be a new service worker registration whose storage key is set to storage key, scope url is set to scope, and update via cache mode is set to updateViaCache.
    // 4. Set registration map[(storage key, scopeString)] to registration.
    // 5. Return registration.

    // FIXME: Is there a way to "ensure but always replace?"
    auto key = RegistrationKey { storage_key, scope.serialize(URL::ExcludeFragment::Yes) };
    (void)s_registrations.set(key, Registration(storage_key, scope, update_via_cache));
    return s_registrations.get(key).value();
}

void Registration::remove(StorageAPI::StorageKey const& key, URL::URL const& scope)
{
    (void)s_registrations.remove({ key, scope.serialize(URL::ExcludeFragment::Yes) });
}

// https://w3c.github.io/ServiceWorker/#get-newest-worker
ServiceWorker* Registration::newest_worker() const
{
    //  FIXME: 1. Run the following steps atomically.

    // 2. Let newestWorker be null.
    // 3. If registration’s installing worker is not null, set newestWorker to registration’s installing worker.
    // 4. If registration’s waiting worker is not null, set newestWorker to registration’s waiting worker.
    // 5. If registration’s active worker is not null, set newestWorker to registration’s active worker.
    // 6. Return newestWorker.
    return m_installing_worker ? m_installing_worker : m_waiting_worker ? m_waiting_worker
                                                                        : m_active_worker;
}

}

namespace AK {
template<>
struct Traits<Web::ServiceWorker::RegistrationKey> : public DefaultTraits<Web::ServiceWorker::RegistrationKey> {
    static unsigned hash(Web::ServiceWorker::RegistrationKey const& key)
    {
        return pair_int_hash(Traits<Web::StorageAPI::StorageKey>::hash(key.key), Traits<ByteString>::hash(key.serialized_scope_url));
    }
};
}
