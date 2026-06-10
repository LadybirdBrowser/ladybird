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

static Optional<String> string_from_devtools_value(JsonValue const& value)
{
    if (value.is_string())
        return value.as_string();
    if (value.is_bool())
        return value.as_bool() ? "true"_string : "false"_string;
    if (value.is_number())
        return MUST(String::formatted("{}", value.get_number_with_precision_loss<double>().release_value()));
    if (value.is_null())
        return {};
    return value.serialized();
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
    if (auto tab = m_tab.strong_ref()) {
        m_storage_change_listener_id = devtools.delegate().add_storage_change_listener(
            tab->description(),
            weak_callback(*this, [](auto& self, DevToolsDelegate::StorageChange change) {
                self.on_storage_changed(move(change));
            }));
    }
}

StorageActor::~StorageActor()
{
    if (m_storage_change_listener_id == 0)
        return;

    if (auto tab = m_tab.strong_ref())
        devtools().delegate().remove_storage_change_listener(tab->description(), m_storage_change_listener_id);
}

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

    if (message.type == "editItem"sv) {
        edit_item(message);
        return;
    }

    if (message.type == "addItem"sv) {
        add_item(message);
        return;
    }

    if (message.type == "removeItem"sv) {
        remove_item(message);
        return;
    }

    if (message.type == "removeAll"sv) {
        remove_all(message);
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

void StorageActor::edit_item(Message const& message)
{
    auto data = get_required_parameter<JsonObject>(message, "data"sv);
    if (!data.has_value())
        return;

    auto requested_host = data->get_string("host"sv);
    if (!requested_host.has_value()) {
        send_missing_parameter_error(message, "host"sv);
        return;
    }

    if (!host().has_value() || *host() != *requested_host) {
        send_response(message, to_storage_operation_result(Error::from_string_literal("Storage host is not available")));
        return;
    }

    auto field = data->get_string("field"sv);
    if (!field.has_value()) {
        send_missing_parameter_error(message, "field"sv);
        return;
    }

    auto items = data->get_object("items"sv);
    if (!items.has_value()) {
        send_missing_parameter_error(message, "items"sv);
        return;
    }

    auto key = items->get_string("name"sv);
    if (!key.has_value()) {
        send_missing_parameter_error(message, "items.name"sv);
        return;
    }

    auto value_json = items->get("value"sv);
    if (!value_json.has_value()) {
        send_missing_parameter_error(message, "items.value"sv);
        return;
    }

    auto value = string_from_devtools_value(*value_json);
    if (!value.has_value()) {
        send_response(message, to_storage_operation_result(Error::from_string_literal("Missing storage value")));
        return;
    }

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    if (*field == "name"sv) {
        auto old_value_json = data->get("oldValue"sv);
        if (!old_value_json.has_value()) {
            send_missing_parameter_error(message, "oldValue"sv);
            return;
        }

        auto old_key = string_from_devtools_value(*old_value_json);
        if (!old_key.has_value()) {
            send_response(message, to_storage_operation_result(Error::from_string_literal("Missing old storage key")));
            return;
        }

        if (*old_key != *key) {
            auto remove_result = devtools().delegate().remove_storage_item(tab->description(), m_storage_endpoint, *requested_host, *old_key);
            if (remove_result.is_error()) {
                send_response(message, to_storage_operation_result(remove_result.release_error()));
                return;
            }
        }
    }

    auto result = devtools().delegate().set_storage_item(tab->description(), m_storage_endpoint, *requested_host, *key, *value);
    if (result.is_error())
        send_response(message, to_storage_operation_result(result.release_error()));
    else
        send_response(message, to_storage_operation_result(ErrorOr<void> {}));
}

void StorageActor::add_item(Message const& message)
{
    auto guid = get_required_parameter<String>(message, "guid"sv);
    if (!guid.has_value())
        return;

    auto requested_host = message.data.get_string("host"sv)
                              .value_or_lazy_evaluated_optional([this] { return host(); });
    if (!requested_host.has_value()) {
        send_response(message, to_storage_operation_result(Error::from_string_literal("No storage host is available for this page")));
        return;
    }

    if (!host().has_value() || *host() != *requested_host) {
        send_response(message, to_storage_operation_result(Error::from_string_literal("Storage host is not available")));
        return;
    }

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    auto result = devtools().delegate().set_storage_item(tab->description(), m_storage_endpoint, *requested_host, *guid, "value"_string);
    if (result.is_error())
        send_response(message, to_storage_operation_result(result.release_error()));
    else
        send_response(message, to_storage_operation_result(ErrorOr<void> {}));
}

void StorageActor::remove_item(Message const& message)
{
    auto requested_host = get_required_parameter<String>(message, "host"sv);
    if (!requested_host.has_value())
        return;

    auto name = get_required_parameter<String>(message, "name"sv);
    if (!name.has_value())
        return;

    if (!host().has_value() || *host() != *requested_host) {
        send_response(message, {});
        return;
    }

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    auto result = devtools().delegate().remove_storage_item(tab->description(), m_storage_endpoint, *requested_host, *name);
    if (result.is_error())
        send_response(message, to_storage_operation_result(result.release_error()));
    else
        send_response(message, {});
}

void StorageActor::remove_all(Message const& message)
{
    auto requested_host = get_required_parameter<String>(message, "host"sv);
    if (!requested_host.has_value())
        return;

    if (!host().has_value() || *host() != *requested_host) {
        send_response(message, {});
        return;
    }

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    auto result = devtools().delegate().clear_storage(tab->description(), m_storage_endpoint, *requested_host);
    if (result.is_error())
        send_response(message, to_storage_operation_result(result.release_error()));
    else
        send_response(message, {});
}

void StorageActor::on_storage_changed(DevToolsDelegate::StorageChange change)
{
    if (change.storage_endpoint != m_storage_endpoint)
        return;

    auto storage_host = host();
    if (!storage_host.has_value() || *storage_host != change.host)
        return;

    if (change.type == DevToolsDelegate::StorageChange::Type::Cleared) {
        send_store_cleared(change);
        return;
    }

    if (!change.key.has_value())
        return;

    send_store_update(change);
}

void StorageActor::send_store_update(DevToolsDelegate::StorageChange const& change)
{
    StringView update_type;
    switch (change.type) {
    case DevToolsDelegate::StorageChange::Type::Added:
        update_type = "added"sv;
        break;
    case DevToolsDelegate::StorageChange::Type::Changed:
        update_type = "changed"sv;
        break;
    case DevToolsDelegate::StorageChange::Type::Deleted:
        update_type = "deleted"sv;
        break;
    case DevToolsDelegate::StorageChange::Type::Cleared:
        VERIFY_NOT_REACHED();
    }

    JsonArray keys;
    keys.must_append(*change.key);

    JsonObject hosts;
    hosts.set(change.host, move(keys));

    JsonObject storage_updates;
    storage_updates.set(resource_key(), move(hosts));

    JsonObject data;
    data.set(update_type, move(storage_updates));

    JsonObject message;
    message.set("type"sv, "storesUpdate"sv);
    message.set("data"sv, move(data));
    send_message(move(message));
}

void StorageActor::send_store_cleared(DevToolsDelegate::StorageChange const& change)
{
    JsonArray hosts;
    hosts.must_append(change.host);

    JsonObject data;
    data.set("clearedHostsOrPaths"sv, move(hosts));

    JsonObject message;
    message.set("type"sv, "storesCleared"sv);
    message.set("data"sv, move(data));
    send_message(move(message));
}

}
