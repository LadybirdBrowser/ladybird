/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Traits.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/ServiceWorkerRegistrationPrototype.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRecord.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration
// This class corresponds to "service worker registration", not "ServiceWorkerRegistration"
// FIXME: This object needs to live at the user-agent level, in LibWebView, not in LibWeb
//        .. And it will need some way to synchronize updates to each 'client' (aka process aka ESO)
class Registration {
    AK_MAKE_NONCOPYABLE(Registration);
    AK_MAKE_DEFAULT_MOVABLE(Registration);

public:
    // https://w3c.github.io/ServiceWorker/#get-registration-algorithm
    static Optional<Registration&> get(StorageAPI::StorageKey const&, Optional<URL::URL> scope);

    // https://w3c.github.io/ServiceWorker/#set-registration-algorithm
    static Registration& set(StorageAPI::StorageKey const&, URL::URL const&, Bindings::ServiceWorkerUpdateViaCache);

    // https://w3c.github.io/ServiceWorker/#scope-match-algorithm
    static Optional<Registration&> match(StorageAPI::StorageKey const&, URL::URL const&);

    static void remove(StorageAPI::StorageKey const&, URL::URL const&);

    bool is_unregistered();

    StorageAPI::StorageKey const& storage_key() const { return m_storage_key; }
    URL::URL const& scope_url() const { return m_scope_url; }
    Bindings::ServiceWorkerUpdateViaCache update_via_cache() const { return m_update_via_cache_mode; }

    void set_last_update_check_time(MonotonicTime time) { m_last_update_check_time = time; }

    ServiceWorkerRecord* newest_worker() const;
    ServiceWorkerRecord* installing_worker() const { return m_installing_worker; }
    ServiceWorkerRecord* waiting_worker() const { return m_waiting_worker; }
    ServiceWorkerRecord* active_worker() const { return m_active_worker; }

    bool is_stale() const;

private:
    Registration(StorageAPI::StorageKey, URL::URL, Bindings::ServiceWorkerUpdateViaCache);

    StorageAPI::StorageKey m_storage_key; // https://w3c.github.io/ServiceWorker/#service-worker-registration-storage-key
    URL::URL m_scope_url;                 // https://w3c.github.io/ServiceWorker/#dfn-scope-url

    // NOTE: These are "service workers", not "ServiceWorker"s - (i.e, not what is exposed over JS)
    ServiceWorkerRecord* m_installing_worker { nullptr }; // https://w3c.github.io/ServiceWorker/#dfn-installing-worker
    ServiceWorkerRecord* m_waiting_worker { nullptr };    // https://w3c.github.io/ServiceWorker/#dfn-waiting-worker
    ServiceWorkerRecord* m_active_worker { nullptr };     // https://w3c.github.io/ServiceWorker/#dfn-active-worker

    Optional<MonotonicTime> m_last_update_check_time;                                                               // https://w3c.github.io/ServiceWorker/#dfn-last-update-check-time
    Bindings::ServiceWorkerUpdateViaCache m_update_via_cache_mode = Bindings::ServiceWorkerUpdateViaCache::Imports; // https://w3c.github.io/ServiceWorker/#dfn-update-via-cache
    // FIXME: A service worker registration has one or more task queues... https://w3c.github.io/ServiceWorker/#dfn-service-worker-registration-task-queue
    // FIXME: Spec bug: A service worker registration has an associated NavigationPreloadManager object.
    //        This can't possibly be true. The association is the other way around.

    bool m_navigation_preload_enabled = { false }; // https://w3c.github.io/ServiceWorker/#service-worker-registration-navigation-preload-enabled-flag
    ByteString m_navigation_preload_header_value;  // https://w3c.github.io/ServiceWorker/#service-worker-registration-navigation-preload-header-value
};

struct RegistrationKey {
    StorageAPI::StorageKey key;
    ByteString serialized_scope_url;

    bool operator==(RegistrationKey const&) const = default;
};

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
