/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/PermissionStatus.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::PermissionsAPI {

// https://w3c.github.io/permissions/#dfn-permission-store-entry
struct PermissionStoreEntry {
    Bindings::PermissionDescriptor descriptor;
    URL::Origin permission_key; // https://w3c.github.io/permissions/#dfn-permission-key
    Bindings::PermissionState state;
};

// https://w3c.github.io/permissions/#dfn-permission-store
class PermissionStore {
public:
    static PermissionStore& the();

    // https://w3c.github.io/permissions/#dfn-get-a-permission-store-entry
    Optional<PermissionStoreEntry> get_permission_store_entry(Bindings::PermissionDescriptor const& descriptor, URL::Origin const& key);

    // https://w3c.github.io/permissions/#dfn-set-a-permission-store-entry
    void set_permission_store_entry(Bindings::PermissionDescriptor const& descriptor, URL::Origin const& key, Bindings::PermissionState state);

    // https://w3c.github.io/permissions/#dfn-remove-a-permission-store-entry
    void remove_permission_store_entry(Bindings::PermissionDescriptor const& descriptor, URL::Origin const& key);

private:
    PermissionStore() = default;

    Vector<PermissionStoreEntry> m_entries;
};

bool permission_key_is_equal_to(Bindings::PermissionDescriptor const& descriptor, URL::Origin const& key1, URL::Origin const& key2);

bool permission_key_comparison_algorithm(URL::Origin const& key1, URL::Origin const& key2);

URL::Origin permission_key_generation_algorithm(URL::Origin const& origin, URL::Origin const&);

}
