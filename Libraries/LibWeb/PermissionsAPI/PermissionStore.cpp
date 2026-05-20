/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PermissionsAPI/PermissionStore.h>

namespace Web::PermissionsAPI {

bool permission_key_is_equal_to(Bindings::PermissionDescriptor const&, URL::Origin const& key1, URL::Origin const& key2)
{
    // FIXME: 1. If key1 is not of descriptor's permission key type or key2 is not of descriptor's permission key type, return false.

    // 2. Return the result of running the permission key comparison algorithm for the feature named by descriptor's name, passing key1 and key2.
    return permission_key_comparison_algorithm(key1, key2);
}

// https://w3c.github.io/permissions/#dfn-permission-key-comparison-algorithm
bool permission_key_comparison_algorithm(URL::Origin const& key1, URL::Origin const& key2)
{
    // 1. Return key1 is same origin with key2.
    return key1.is_same_origin(key2);
}

// https://w3c.github.io/permissions/#dfn-permission-key-generation-algorithm
URL::Origin permission_key_generation_algorithm(URL::Origin const& origin, URL::Origin const&)
{
    // Takes an origin origin and an origin embedded origin, and returns a new permission key.
    // If unspecified, this defaults to the default permission key generation algorithm.
    // A feature that specifies a custom permission key generation algorithm MUST also specify a permission key comparison algorithm.

    // 1. Return origin.
    return origin;

    // Note: Permission Delegation
    // Most powerful features grant permission to the top-level origin and delegate access to the requesting document via Permissions Policy.
    // This is known as permission delegation.
}

// FIXME: This should be store at the user-agent level (IPC to the browser process)
PermissionStore& PermissionStore::the()
{
    static PermissionStore s_the;
    return s_the;
}

Optional<PermissionStoreEntry> PermissionStore::get_permission_store_entry(Bindings::PermissionDescriptor const& descriptor, URL::Origin const& key)
{
    // 1. If the user agent's permission store contains an entry whose descriptor is descriptor, and whose key is equal to key given descriptor, return that entry.
    for (auto const& entry : m_entries) {
        if (entry.descriptor.name == descriptor.name && permission_key_is_equal_to(descriptor, entry.permission_key, key))
            return entry;
    }

    // 2. Return null.
    return {};
}

void PermissionStore::set_permission_store_entry(Bindings::PermissionDescriptor const& descriptor, URL::Origin const& key, Bindings::PermissionState state)
{
    // 1. Let newEntry be a new permission store entry whose descriptor is descriptor, and whose key is key, and whose state is state.
    PermissionStoreEntry new_entry { descriptor, key, state };

    // 2. If the user agent's permission store contains an entry whose descriptor is descriptor, and whose key is equal to key given descriptor, replace that entry with newEntry and abort these steps.
    for (auto& entry : m_entries) {
        if (entry.descriptor.name == descriptor.name && permission_key_is_equal_to(descriptor, entry.permission_key, key)) {
            entry = new_entry;
            return;
        }
    }

    // 3. Append newEntry to the user agent's permission store.
    m_entries.append(move(new_entry));
}

void PermissionStore::remove_permission_store_entry(Bindings::PermissionDescriptor const& descriptor, URL::Origin const& key)
{
    // 1. Remove the entry whose descriptor is descriptor, and whose key is equal to key given descriptor, from the user agent's permission store.
    m_entries.remove_all_matching([&](auto const& entry) {
        return entry.descriptor.name == descriptor.name && permission_key_is_equal_to(descriptor, entry.permission_key, key);
    });
}

}
