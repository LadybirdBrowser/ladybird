/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/StorageAreaRegistry.h>

namespace WebView {

StorageAreaRegistry& StorageAreaRegistry::the()
{
    // Outlives the process, rather than running a destructor at exit.
    static auto* registry = new StorageAreaRegistry;
    return *registry;
}

void StorageAreaRegistry::remove_areas_of(Badge<StorageJar>, StorageJar& jar)
{
    auto areas = m_areas.take(&jar);
    if (!areas.has_value())
        return;
    for (auto const& entry : *areas) {
        for (auto const& listener_id : entry.value.listener_ids)
            m_live_connections.remove(listener_id.value);
    }
}

StorageAreaRegistry::Area* StorageAreaRegistry::area_for(StorageJar& jar, StorageBottleLocation const& location)
{
    auto areas = m_areas.get(&jar);
    if (!areas.has_value())
        return nullptr;
    auto area = areas->get(location);
    if (!area.has_value())
        return nullptr;
    return &*area;
}

Optional<StorageAreaRegistry::Connection> StorageAreaRegistry::connect(StorageAreaListener& listener, StorageJar& jar, StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (storage_endpoint != StorageEndpointType::LocalStorage && storage_endpoint != StorageEndpointType::SessionStorage)
        return {};

    StorageBottleLocation location { storage_endpoint, storage_key };
    auto& area = m_areas.ensure(&jar).ensure(location, [&] { return Area { &jar, storage_endpoint, storage_key, {} }; });

    // A listener that connects again to an area it already mirrors has let its old mirror go; anything still
    // addressed to that one is dropped from here on.
    if (auto previous = area.listener_ids.get(&listener); previous.has_value())
        m_live_connections.remove(*previous);

    Web::StorageAPI::StorageAreaId id { m_next_id++ };
    area.listener_ids.set(&listener, id);
    m_live_connections.set(id, LiveConnection { &jar, location, &listener });
    m_issued_ids.ensure(&listener).set(id);

    jar.mark_accessed(storage_endpoint, storage_key);
    return Connection { id, jar.get_all_items(storage_endpoint, storage_key), jar.bottle_usage(storage_endpoint, storage_key) };
}

void StorageAreaRegistry::disconnect(StorageAreaListener& listener, Web::StorageAPI::StorageAreaId id)
{
    auto live = m_live_connections.get(id);
    if (!live.has_value() || live->listener != &listener)
        return;
    auto connection = *live;
    m_live_connections.remove(id);

    auto* area = area_for(*connection.jar, connection.location);
    if (!area)
        return;
    if (auto current = area->listener_ids.get(&listener); current.has_value() && *current == id)
        area->listener_ids.remove(&listener);
    if (!area->listener_ids.is_empty())
        return;

    // An area lives as long as something mirrors it.
    auto areas = m_areas.get(connection.jar);
    areas->remove(connection.location);
    if (areas->is_empty())
        m_areas.remove(connection.jar);
}

void StorageAreaRegistry::remove_listener(StorageAreaListener& listener)
{
    auto ids = m_issued_ids.take(&listener);
    if (!ids.has_value())
        return;
    for (auto id : *ids)
        disconnect(listener, id);
}

bool StorageAreaRegistry::was_issued_to(StorageAreaListener const& listener, Web::StorageAPI::StorageAreaId id) const
{
    auto ids = m_issued_ids.get(&listener);
    return ids.has_value() && ids->contains(id);
}

StorageAreaRegistry::Area* StorageAreaRegistry::connected_area(StorageAreaListener& listener, Web::StorageAPI::StorageAreaId id)
{
    auto live = m_live_connections.get(id);
    if (!live.has_value() || live->listener != &listener)
        return nullptr;
    return area_for(*live->jar, live->location);
}

void StorageAreaRegistry::set_item(StorageAreaListener& listener, Web::StorageAPI::StorageAreaId id, Utf16String const& key, Utf16String const& value, String const& url)
{
    auto* area = connected_area(listener, id);
    if (!area)
        return;

    m_current_write = CurrentWrite { &listener, url };
    auto result = area->jar->set_item(area->storage_endpoint, area->storage_key, key, value);
    m_current_write.clear();

    // The writer applied the write to its mirror before sending it, on the strength of the same quota rule — so a
    // refusal means another process filled the bottle in between, and the writer has to reload it to agree again.
    if (result.has<StorageOperationError>())
        listener.storage_area_write_refused(id, area->jar->get_all_items(area->storage_endpoint, area->storage_key));
    else
        listener.storage_area_write_acknowledged(id);
}

void StorageAreaRegistry::remove_item(StorageAreaListener& listener, Web::StorageAPI::StorageAreaId id, Utf16String const& key, String const& url)
{
    auto* area = connected_area(listener, id);
    if (!area)
        return;

    m_current_write = CurrentWrite { &listener, url };
    area->jar->remove_item(area->storage_endpoint, area->storage_key, key);
    m_current_write.clear();
    listener.storage_area_write_acknowledged(id);
}

void StorageAreaRegistry::clear(StorageAreaListener& listener, Web::StorageAPI::StorageAreaId id, String const& url)
{
    auto* area = connected_area(listener, id);
    if (!area)
        return;

    m_current_write = CurrentWrite { &listener, url };
    area->jar->clear_storage_key(area->storage_endpoint, area->storage_key);
    m_current_write.clear();
    listener.storage_area_write_acknowledged(id);
}

void StorageAreaRegistry::did_change(Badge<StorageJar>, StorageJar& jar, StorageChange const& change)
{
    Optional<String> writer_url;
    StorageAreaListener* writer = nullptr;
    if (m_current_write.has_value()) {
        writer = m_current_write->writer;
        writer_url = m_current_write->url;
    }

    if (auto* area = area_for(jar, { change.storage_endpoint, change.storage_key })) {
        // The writer already applied its own change; everyone else learns of it here, each under its own id.
        for (auto const& listener_id : area->listener_ids) {
            auto* listener = listener_id.key;
            if (listener == writer)
                continue;
            if (change.type == StorageChange::Type::Reset)
                listener->storage_area_reset(listener_id.value, jar.get_all_items(change.storage_endpoint, change.storage_key));
            else
                listener->storage_area_changed(listener_id.value, change.key, change.old_value, change.new_value, writer_url.value_or(change.storage_key));
        }
    }

    if (on_change_for_devtools)
        on_change_for_devtools(jar, change);
}

}
