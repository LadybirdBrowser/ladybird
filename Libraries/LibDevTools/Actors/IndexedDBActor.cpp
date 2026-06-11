/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/IndexedDBActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibDevTools/StorageHelpers.h>

namespace DevTools {

NonnullRefPtr<IndexedDBActor> IndexedDBActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new IndexedDBActor(devtools, move(name), move(tab)));
}

IndexedDBActor::IndexedDBActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
    if (auto tab = m_tab.strong_ref()) {
        m_indexed_database_change_listener_id = devtools.delegate().add_indexed_database_change_listener(
            tab->description(),
            weak_callback(*this, [](auto& self, JsonObject update) {
                self.on_indexed_database_changed(move(update));
            }));
    }
}

IndexedDBActor::~IndexedDBActor()
{
    if (m_indexed_database_change_listener_id == 0)
        return;

    if (auto tab = m_tab.strong_ref())
        devtools().delegate().remove_indexed_database_change_listener(tab->description(), m_indexed_database_change_listener_id);
}

JsonObject IndexedDBActor::serialize_storage(JsonObject hosts) const
{
    if (hosts.is_empty()) {
        if (auto tab = m_tab.strong_ref()) {
            if (auto host = storage_host_for_url(tab->description().url); host.has_value())
                hosts.set(*host, JsonArray {});
        }
    }

    JsonObject traits;
    traits.set("supportsAddItem"sv, false);
    traits.set("supportsRemoveAll"sv, true);
    traits.set("supportsRemoveAllSessionCookies"sv, false);
    traits.set("supportsRemoveItem"sv, true);

    JsonObject storage;
    storage.set("actor"sv, name());
    if (auto tab = m_tab.strong_ref()) {
        storage.set("browsingContextID"sv, tab->description().id);
        storage.set("innerWindowId"sv, tab->inner_window_id());
        storage.set("resourceId"sv, MUST(String::formatted("indexed-db-{}", tab->inner_window_id())));
    }
    storage.set("hosts"sv, move(hosts));
    storage.set("resourceKey"sv, "indexedDB"sv);
    storage.set("traits"sv, move(traits));
    return storage;
}

void IndexedDBActor::get_storage_resource(Function<void(JsonObject)> callback)
{
    auto tab = m_tab.strong_ref();
    if (!tab) {
        callback(serialize_storage({}));
        return;
    }

    devtools().delegate().inspect_indexed_database_storage(tab->description(),
        [weak_self = make_weak_ptr<IndexedDBActor>(), callback = move(callback)](ErrorOr<JsonObject> hosts_or_error) mutable {
            auto self = weak_self.strong_ref();
            if (!self)
                return;

            callback(self->serialize_storage(hosts_or_error.is_error() ? JsonObject {} : hosts_or_error.release_value()));
        });
}

void IndexedDBActor::handle_message(Message const& message)
{
    if (message.type == "getFields"sv) {
        get_fields(message);
        return;
    }

    if (message.type == "getStoreObjects"sv) {
        get_store_objects(message);
        return;
    }

    if (message.type == "removeDatabase"sv) {
        remove_database(message);
        return;
    }

    if (message.type == "removeAll"sv) {
        remove_all(message);
        return;
    }

    if (message.type == "removeItem"sv) {
        remove_item(message);
        return;
    }

    send_unrecognized_packet_type_error(message);
}

void IndexedDBActor::get_fields(Message const& message)
{
    auto sub_type = message.data.get_string("subType"sv);

    JsonArray fields;
    if (sub_type == "database"sv) {
        fields.must_append(define_storage_field("objectStore"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("keyPath"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("autoIncrement"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("indexes"sv, StorageFieldType::Immutable));
    } else if (sub_type == "object store"sv) {
        fields.must_append(define_storage_field("name"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("value"sv, StorageFieldType::Immutable));
    } else {
        fields.must_append(define_storage_field("uniqueKey"sv, StorageFieldType::Private));
        fields.must_append(define_storage_field("db"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("storage"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("origin"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("version"sv, StorageFieldType::Immutable));
        fields.must_append(define_storage_field("objectStores"sv, StorageFieldType::Immutable));
    }

    JsonObject response;
    response.set("value"sv, move(fields));
    send_response(message, move(response));
}

void IndexedDBActor::send_inspection_error(Message const& message, Error const& error)
{
    JsonObject response;
    response.set("error"sv, "indexedDBInspectionFailed"sv);
    response.set("message"sv, error.string_literal());
    send_response(message, move(response));
}

void IndexedDBActor::get_store_objects(Message const& message)
{
    auto host = get_required_parameter<String>(message, "host"sv);
    if (!host.has_value())
        return;

    auto tab = m_tab.strong_ref();
    if (!tab) {
        send_inspection_error(message, Error::from_string_literal("Unable to locate tab"));
        return;
    }

    Optional<JsonArray> names;
    if (auto names_array = message.data.get_array("names"sv); names_array.has_value())
        names = *names_array;
    auto options = message.data.get_object("options"sv).value_or({});

    devtools().delegate().inspect_indexed_database_objects(tab->description(), *host, move(names), move(options),
        [weak_self = make_weak_ptr<IndexedDBActor>(), message_id = message.id](ErrorOr<JsonObject> result) mutable {
            auto self = weak_self.strong_ref();
            if (!self)
                return;

            if (result.is_error()) {
                self->send_inspection_error({ .id = message_id }, result.error());
                return;
            }

            self->send_response({ .id = message_id }, result.release_value());
        });
}

void IndexedDBActor::remove_database(Message const& message)
{
    auto host = get_required_parameter<String>(message, "host"sv);
    auto name = get_required_parameter<String>(message, "name"sv);
    if (!host.has_value() || !name.has_value())
        return;

    auto tab = m_tab.strong_ref();
    if (!tab) {
        send_inspection_error(message, Error::from_string_literal("Unable to locate tab"));
        return;
    }

    devtools().delegate().delete_indexed_database(tab->description(), *host, *name,
        [weak_self = make_weak_ptr<IndexedDBActor>(), message_id = message.id, host = *host, name = *name](ErrorOr<JsonObject> result) mutable {
            auto self = weak_self.strong_ref();
            if (!self)
                return;

            if (result.is_error()) {
                self->send_inspection_error({ .id = message_id }, result.error());
                return;
            }

            auto response = result.release_value();
            auto blocked = response.get_bool("blocked"sv).value_or(false);
            self->send_response({ .id = message_id }, move(response));
            if (!blocked) {
                JsonArray path;
                path.must_append(name);
                self->send_indexed_database_update("deleted"sv, host, path.serialized());
            }
        });
}

void IndexedDBActor::remove_all(Message const& message)
{
    auto host = get_required_parameter<String>(message, "host"sv);
    auto name = get_required_parameter<String>(message, "name"sv);
    if (!host.has_value() || !name.has_value())
        return;

    auto tab = m_tab.strong_ref();
    if (!tab) {
        send_inspection_error(message, Error::from_string_literal("Unable to locate tab"));
        return;
    }

    devtools().delegate().clear_indexed_database_object_store(tab->description(), *host, *name,
        [weak_self = make_weak_ptr<IndexedDBActor>(), message_id = message.id, host = *host, name = *name](ErrorOr<JsonObject> result) mutable {
            auto self = weak_self.strong_ref();
            if (!self)
                return;

            if (result.is_error()) {
                self->send_inspection_error({ .id = message_id }, result.error());
                return;
            }

            auto response = result.release_value();
            self->send_response({ .id = message_id }, move(response));
            self->send_indexed_database_clear(host, name);
        });
}

void IndexedDBActor::remove_item(Message const& message)
{
    auto host = get_required_parameter<String>(message, "host"sv);
    auto name = get_required_parameter<String>(message, "name"sv);
    if (!host.has_value() || !name.has_value())
        return;

    auto tab = m_tab.strong_ref();
    if (!tab) {
        send_inspection_error(message, Error::from_string_literal("Unable to locate tab"));
        return;
    }

    devtools().delegate().delete_indexed_database_record(tab->description(), *host, *name,
        [weak_self = make_weak_ptr<IndexedDBActor>(), message_id = message.id, host = *host, name = *name](ErrorOr<JsonObject> result) mutable {
            auto self = weak_self.strong_ref();
            if (!self)
                return;

            if (result.is_error()) {
                self->send_inspection_error({ .id = message_id }, result.error());
                return;
            }

            auto response = result.release_value();
            self->send_response({ .id = message_id }, move(response));
            self->send_indexed_database_update("deleted"sv, host, name);
        });
}

void IndexedDBActor::send_indexed_database_update(StringView update_type, String const& host, String const& name)
{
    JsonArray paths;
    paths.must_append(name);

    JsonObject hosts;
    hosts.set(host, move(paths));

    JsonObject indexed_database;
    indexed_database.set("indexedDB"sv, move(hosts));

    JsonObject update;
    update.set(update_type, move(indexed_database));

    on_indexed_database_changed(move(update));
}

void IndexedDBActor::send_indexed_database_clear(String const& host, String const& name)
{
    JsonArray paths;
    paths.must_append(name);

    JsonObject hosts;
    hosts.set(host, move(paths));

    JsonObject data;
    data.set("clearedHostsOrPaths"sv, move(hosts));

    JsonObject message;
    message.set("type"sv, "storesCleared"sv);
    message.set("data"sv, move(data));
    send_message(move(message));
}

void IndexedDBActor::on_indexed_database_changed(JsonObject update)
{
    JsonObject message;
    message.set("type"sv, "storesUpdate"sv);
    message.set("data"sv, move(update));
    send_message(move(message));
}

}
