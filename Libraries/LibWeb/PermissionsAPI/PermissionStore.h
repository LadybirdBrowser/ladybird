/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    // The UI process owns the user agent's permission decisions, and sends each process that reads them a snapshot of
    // every site's state; a snapshot older than the one in hand is ignored.
    void apply_snapshot(Web::Permissions::PermissionSnapshot);
    Web::Permissions::EffectiveState state_for(Web::Permissions::PermissionName, URL::Origin const& key) const;

    void register_status(PermissionStatus&);

private:
    PermissionStore() = default;

    Vector<PermissionStoreEntry> m_entries;
    Optional<Web::Permissions::PermissionSnapshot> m_snapshot;
    Vector<GC::Weak<PermissionStatus>> m_statuses;
};

bool permission_key_is_equal_to(PermissionDescriptor const& descriptor, URL::Origin const& key1, URL::Origin const& key2);

bool permission_key_comparison_algorithm(URL::Origin const& key1, URL::Origin const& key2);

URL::Origin permission_key_generation_algorithm(URL::Origin const& origin, URL::Origin const&);

}
