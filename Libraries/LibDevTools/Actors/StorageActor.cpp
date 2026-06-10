/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <LibDevTools/Actors/StorageActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibDevTools/StorageHelpers.h>

namespace DevTools {

static constexpr auto max_store_object_count = 50uz;

static JsonObject storage_field(StringView name)
{
    JsonObject field;
    field.set("name"sv, name);
    field.set("editable"sv, true);
    return field;
}

static JsonObject serialize_storage_item(DevToolsDelegate::StorageItem const& item)
{
    JsonObject object;
    object.set("name"sv, item.name);
    object.set("value"sv, item.value);
    return object;
}

NonnullRefPtr<StorageActor> StorageActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, Web::StorageAPI::StorageEndpointType storage_endpoint)
{
    return adopt_ref(*new StorageActor(devtools, move(name), move(tab), storage_endpoint));
}

StorageActor::StorageActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, Web::StorageAPI::StorageEndpointType storage_endpoint)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_storage_endpoint(storage_endpoint)
{
}

StorageActor::~StorageActor() = default;

StringView StorageActor::resource_type() const
{
    if (m_storage_endpoint == Web::StorageAPI::StorageEndpointType::LocalStorage)
        return "local-storage"sv;
    VERIFY(m_storage_endpoint == Web::StorageAPI::StorageEndpointType::SessionStorage);
    return "session-storage"sv;
}

StringView StorageActor::resource_key() const
{
    if (m_storage_endpoint == Web::StorageAPI::StorageEndpointType::LocalStorage)
        return "localStorage"sv;
    VERIFY(m_storage_endpoint == Web::StorageAPI::StorageEndpointType::SessionStorage);
    return "sessionStorage"sv;
}

Optional<String> StorageActor::host() const
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};
    return storage_host_for_url(tab->description().url);
}

JsonObject StorageActor::serialize_storage() const
{
    JsonObject hosts;
    if (auto storage_host = host(); storage_host.has_value())
        hosts.set(*storage_host, JsonArray {});

    JsonObject traits;
    traits.set("supportsAddItem"sv, true);
    traits.set("supportsRemoveAll"sv, true);
    traits.set("supportsRemoveAllSessionCookies"sv, false);
    traits.set("supportsRemoveItem"sv, true);

    JsonObject storage;
    storage.set("actor"sv, name());
    if (auto tab = m_tab.strong_ref()) {
        storage.set("browsingContextID"sv, tab->description().id);
        storage.set("innerWindowId"sv, tab->inner_window_id());
        storage.set("resourceId"sv, MUST(String::formatted("{}-{}", resource_key(), tab->inner_window_id())));
    }
    storage.set("hosts"sv, move(hosts));
    storage.set("resourceKey"sv, resource_key());
    storage.set("traits"sv, move(traits));
    return storage;
}

void StorageActor::handle_message(Message const& message)
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

void StorageActor::get_fields(Message const& message)
{
    JsonArray fields;
    fields.must_append(storage_field("name"sv));
    fields.must_append(storage_field("value"sv));

    JsonObject response;
    response.set("value"sv, move(fields));
    send_response(message, move(response));
}

void StorageActor::get_store_objects(Message const& message)
{
    auto requested_host_ref = get_required_parameter<String>(message, "host"sv);
    if (!requested_host_ref.has_value())
        return;
    String requested_host = *requested_host_ref;

    auto names_value = message.data.get("names"sv);
    Optional<JsonArray> requested_names;
    if (names_value.has_value() && names_value->is_array())
        requested_names = names_value->as_array();

    JsonObject options;
    auto options_value = message.data.get("options"sv);
    if (options_value.has_value() && options_value->is_object())
        options = options_value->as_object();

    auto tab = m_tab.strong_ref();
    if (!tab) {
        send_store_objects(message, requested_host, move(requested_names), move(options), Vector<DevToolsDelegate::StorageItem> {});
        return;
    }

    devtools().delegate().inspect_storage(
        tab->description(),
        m_storage_endpoint,
        weak_callback(*this, [message, requested_host, requested_names, options](auto& self, ErrorOr<Vector<DevToolsDelegate::StorageItem>> storage_items) {
            self.send_store_objects(message, requested_host, requested_names, options, move(storage_items));
        }));
}

void StorageActor::send_store_objects(Message const& message, Optional<String> requested_host, Optional<JsonArray> requested_names, JsonObject options, ErrorOr<Vector<DevToolsDelegate::StorageItem>> storage_items_or_error)
{
    if (storage_items_or_error.is_error()) {
        JsonObject response;
        response.set("error"sv, "storageError"sv);
        response.set("message"sv, storage_items_or_error.error().string_literal());
        send_response(message, move(response));
        return;
    }

    Vector<DevToolsDelegate::StorageItem> storage_items = storage_items_or_error.release_value();
    if (!host().has_value() || !requested_host.has_value() || *host() != *requested_host)
        storage_items.clear();

    if (requested_names.has_value()) {
        storage_items.remove_all_matching([&](auto const& item) {
            return !requested_names->values().contains([&](auto const& value) {
                return value.is_string() && value.as_string() == item.name;
            });
        });
    }

    quick_sort(storage_items, [](auto const& a, auto const& b) {
        return a.name < b.name;
    });

    auto total = storage_items.size();
    auto offset = options.get_integer<size_t>("offset"sv).value_or(0);
    auto size = options.get_integer<size_t>("size"sv).value_or(max_store_object_count);
    if (size > max_store_object_count)
        size = max_store_object_count;

    JsonArray data;
    if (offset > total) {
        offset = total;
    } else {
        auto end = min(total, offset + size);
        for (auto i = offset; i < end; ++i)
            data.must_append(serialize_storage_item(storage_items[i]));
    }

    JsonObject response;
    response.set("offset"sv, offset);
    response.set("total"sv, total);
    response.set("data"sv, move(data));
    send_response(message, move(response));
}

}
