/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGC/Weak.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWebCommon/Permissions/Permissions.h>

namespace Web::PermissionsAPI {

class PermissionStatus;

// https://w3c.github.io/permissions/#dfn-permission-store-entry
struct PermissionStoreEntry {
    PermissionDescriptor descriptor;
    URL::Origin permission_key; // https://w3c.github.io/permissions/#dfn-permission-key
    PermissionState state;
};

// https://w3c.github.io/permissions/#dfn-permission-store
class WEB_API PermissionStore {
public:
    AK_ALLOC_WITH_KMALLOC;

    static PermissionStore& the();

    // https://w3c.github.io/permissions/#dfn-get-a-permission-store-entry
    Optional<PermissionStoreEntry> get_permission_store_entry(PermissionDescriptor const& descriptor, URL::Origin const& key);

    // https://w3c.github.io/permissions/#dfn-set-a-permission-store-entry
    void set_permission_store_entry(PermissionDescriptor const& descriptor, URL::Origin const& key, PermissionState state);

    // The UI process owns the user agent's permission decisions. It tells this process the states of the sites it
    // shows: the first time the process reads one, through the page reading it, and again whenever one changes. Of
    // each site, the newest state wins.
    enum class Notify : bool {
        No,
        Yes,
    };
    void apply_snapshot(Web::Permissions::PermissionSnapshot, Notify = Notify::Yes);
    Web::Permissions::EffectiveState state_for(Web::Permissions::PermissionName, URL::Origin const& key, Page&);

    void register_status(PermissionStatus&);

    // The sites this process holds decisions for.
    Vector<String> cached_origins_for_testing() const;

private:
    PermissionStore() = default;

    Vector<PermissionStoreEntry> m_entries;

    struct CachedSite {
        u64 revision { 0 };
        Vector<Web::Permissions::PermissionSnapshotEntry> entries;
    };
    HashMap<String, CachedSite> m_sites;
    Optional<u64> m_defaults_revision;
    Web::Permissions::EffectiveState m_geolocation_default { Web::Permissions::EffectiveState::Prompt };
    Web::Permissions::EffectiveState m_microphone_default { Web::Permissions::EffectiveState::Prompt };
    Vector<GC::Weak<PermissionStatus>> m_statuses;
};

bool permission_key_is_equal_to(PermissionDescriptor const& descriptor, URL::Origin const& key1, URL::Origin const& key2);

bool permission_key_comparison_algorithm(URL::Origin const& key1, URL::Origin const& key2);

URL::Origin permission_key_generation_algorithm(URL::Origin const& origin, URL::Origin const&);

}
