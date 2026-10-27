/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Root.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/PermissionStatus.h>
#include <LibWeb/PermissionsAPI/PermissionStore.h>

namespace Web::PermissionsAPI {

bool permission_key_is_equal_to(PermissionDescriptor const&, URL::Origin const& key1, URL::Origin const& key2)
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
    static auto& s_the = *new PermissionStore;
    return s_the;
}

Optional<PermissionStoreEntry> PermissionStore::get_permission_store_entry(PermissionDescriptor const& descriptor, URL::Origin const& key)
{
    // 1. If the user agent's permission store contains an entry whose descriptor is descriptor, and whose key is equal to key given descriptor, return that entry.
    for (auto const& entry : m_entries) {
        if (entry.descriptor.name == descriptor.name && permission_key_is_equal_to(descriptor, entry.permission_key, key))
            return entry;
    }

    // 2. Return null.
    return {};
}

void PermissionStore::set_permission_store_entry(PermissionDescriptor const& descriptor, URL::Origin const& key, PermissionState state)
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

void PermissionStore::apply_snapshot(Web::Permissions::PermissionSnapshot snapshot, Notify notify)
{
    auto changed = false;

    if (!m_defaults_revision.has_value() || snapshot.revision > *m_defaults_revision) {
        m_defaults_revision = snapshot.revision;
        m_geolocation_default = snapshot.geolocation_default;
        m_microphone_default = snapshot.microphone_default;
        changed = true;
    }

    for (auto const& origin : snapshot.origins) {
        auto key = origin.serialize();
        if (auto cached = m_sites.get(key); cached.has_value() && snapshot.revision <= cached->revision)
            continue;

        CachedSite site { .revision = snapshot.revision, .entries = {} };
        for (auto const& entry : snapshot.entries) {
            if (entry.top_level_origin.is_same_origin(origin))
                site.entries.append(entry);
        }
        m_sites.set(key, move(site));
        changed = true;
    }

    if (!changed || notify == Notify::No)
        return;

    // NB: Updating a status allocates, and a collection then could clear another status's weak reference, so every
    //     live status is rooted first.
    m_statuses.remove_all_matching([](auto const& status) { return !status; });
    Vector<GC::Root<PermissionStatus>> statuses;
    for (auto const& status : m_statuses) {
        if (status)
            statuses.append(GC::make_root(status.ptr()));
    }
    for (auto const& status : statuses)
        status->update_from_store();

    // A document whose microphone permission went away loses its live microphone tracks.
    for (auto navigable : HTML::all_local_navigables()) {
        auto document = navigable->active_document();
        if (!document || !document->window())
            continue;
        if (auto navigator = document->window()->navigator_if_created(); navigator) {
            if (auto media_devices = navigator->media_devices_if_created())
                media_devices->end_tracks_without_permission();
        }
    }
}

Web::Permissions::EffectiveState PermissionStore::state_for(Web::Permissions::PermissionName name, URL::Origin const& key, Page& page)
{
    auto default_state = [&] {
        switch (name) {
        case Web::Permissions::PermissionName::Geolocation:
            return m_geolocation_default;
        case Web::Permissions::PermissionName::Microphone:
            return m_microphone_default;
        }
        VERIFY_NOT_REACHED();
    };

    if (key.is_opaque())
        return default_state();

    auto serialized_key = key.serialize();
    if (!m_sites.contains(serialized_key)) {
        // No status has seen this site's state yet, so there's nothing to tell them about.
        if (auto snapshot = page.client().page_did_request_permission_snapshot(key); snapshot.has_value())
            apply_snapshot(snapshot.release_value(), Notify::No);
    }

    if (auto site = m_sites.get(serialized_key); site.has_value()) {
        for (auto const& entry : site->entries) {
            if (entry.name == name)
                return entry.state;
        }
    }
    return default_state();
}

Vector<String> PermissionStore::cached_origins_for_testing() const
{
    Vector<String> origins;
    for (auto const& it : m_sites) {
        if (!it.value.entries.is_empty())
            origins.append(it.key);
    }
    return origins;
}

void PermissionStore::register_status(PermissionStatus& status)
{
    m_statuses.append(status);
}

}
