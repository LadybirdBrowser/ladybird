/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/CookiesActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/StorageHelpers.h>

namespace DevTools {

NonnullRefPtr<CookiesActor> CookiesActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new CookiesActor(devtools, move(name), move(tab)));
}

CookiesActor::CookiesActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

CookiesActor::~CookiesActor() = default;

Optional<String> CookiesActor::host() const
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};
    return storage_host_for_url(tab->description().url);
}

JsonObject CookiesActor::serialize_storage() const
{
    JsonObject hosts;
    if (auto storage_host = host(); storage_host.has_value())
        hosts.set(*storage_host, JsonArray {});

    JsonObject traits;
    traits.set("supportsAddItem"sv, false);
    traits.set("supportsRemoveAll"sv, false);
    traits.set("supportsRemoveAllSessionCookies"sv, false);
    traits.set("supportsRemoveItem"sv, false);

    JsonObject storage;
    storage.set("actor"sv, name());
    if (auto tab = m_tab.strong_ref()) {
        storage.set("browsingContextID"sv, tab->description().id);
        storage.set("innerWindowId"sv, tab->inner_window_id());
        storage.set("resourceId"sv, MUST(String::formatted("cookies-{}", tab->inner_window_id())));
    }
    storage.set("hosts"sv, move(hosts));
    storage.set("resourceKey"sv, "cookies"sv);
    storage.set("traits"sv, move(traits));
    return storage;
}

void CookiesActor::handle_message(Message const& message)
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

void CookiesActor::get_fields(Message const& message)
{
    enum class FieldState : u8 {
        Immutable,
        Editable,
        Hidden,
        Private,
    };
    auto make_field = [](StringView name, FieldState state) {
        JsonObject field;
        field.set("name"sv, name);
        field.set("editable"sv, state == FieldState::Editable);
        field.set("hidden"sv, state == FieldState::Hidden);
        field.set("private"sv, state == FieldState::Private);
        return field;
    };

    JsonArray fields;
    fields.must_append(make_field("uniqueKey"sv, FieldState::Private));
    fields.must_append(make_field("name"sv, FieldState::Editable));
    fields.must_append(make_field("value"sv, FieldState::Editable));
    fields.must_append(make_field("host"sv, FieldState::Editable));
    fields.must_append(make_field("path"sv, FieldState::Editable));
    fields.must_append(make_field("expires"sv, FieldState::Editable));
    fields.must_append(make_field("size"sv, FieldState::Immutable));
    fields.must_append(make_field("isHttpOnly"sv, FieldState::Editable));
    fields.must_append(make_field("isSecure"sv, FieldState::Editable));
    fields.must_append(make_field("sameSite"sv, FieldState::Immutable));
    fields.must_append(make_field("lastAccessed"sv, FieldState::Immutable));
    fields.must_append(make_field("creationTime"sv, FieldState::Hidden));
    fields.must_append(make_field("updateTime"sv, FieldState::Hidden));
    fields.must_append(make_field("hostOnly"sv, FieldState::Hidden));
    fields.must_append(make_field("partitionKey"sv, FieldState::Immutable));

    JsonObject response;
    response.set("value"sv, move(fields));
    send_response(message, move(response));
}

void CookiesActor::get_store_objects(Message const& message)
{
    JsonObject response;
    response.set("offset"sv, 0);
    response.set("total"sv, 0);
    response.set("data"sv, JsonArray {});
    send_response(message, move(response));
}

}
