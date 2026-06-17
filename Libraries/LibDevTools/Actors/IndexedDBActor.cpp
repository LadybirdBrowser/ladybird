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
}

IndexedDBActor::~IndexedDBActor() = default;

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

}
