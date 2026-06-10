/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <LibDevTools/Actors/CookiesActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

namespace DevTools {

static constexpr auto cookie_unique_key_separator = "{9d414cc5-8319-0a04-0586-c0a6ae01670a}"sv;
static constexpr auto max_store_object_count = 50uz;

static bool cookie_matches_storage_host(HTTP::Cookie::Cookie const& cookie, String const& storage_host)
{
    auto host_name = storage_host_name(storage_host);
    if (!host_name.has_value())
        return false;

    if (cookie.host_only)
        return cookie.domain == *host_name;

    return HTTP::Cookie::domain_matches(*host_name, cookie.domain);
}

static String cookie_unique_key(HTTP::Cookie::Cookie const& cookie)
{
    return MUST(String::formatted("{}{}{}{}{}{}",
        cookie.name,
        cookie_unique_key_separator,
        cookie.domain,
        cookie_unique_key_separator,
        cookie.path,
        cookie_unique_key_separator));
}

static String same_site_for_devtools(HTTP::Cookie::SameSite same_site)
{
    if (same_site == HTTP::Cookie::SameSite::Default)
        return {};
    return MUST(String::from_utf8(HTTP::Cookie::same_site_to_string(same_site)));
}

static JsonObject serialize_cookie(HTTP::Cookie::Cookie const& cookie)
{
    JsonObject object;
    object.set("uniqueKey"sv, cookie_unique_key(cookie));
    object.set("name"sv, cookie.name);
    object.set("value"sv, cookie.value);
    object.set("host"sv, cookie.domain);
    object.set("path"sv, cookie.path);
    object.set("expires"sv, cookie.persistent ? cookie.expiry_time.milliseconds_since_epoch() : 0);
    object.set("size"sv, cookie.name.bytes().size() + cookie.value.bytes().size());
    object.set("isHttpOnly"sv, cookie.http_only);
    object.set("isSecure"sv, cookie.secure);
    object.set("sameSite"sv, same_site_for_devtools(cookie.same_site));
    object.set("lastAccessed"sv, cookie.last_access_time.milliseconds_since_epoch());
    object.set("creationTime"sv, cookie.creation_time.milliseconds_since_epoch());
    object.set("updateTime"sv, cookie.creation_time.milliseconds_since_epoch());
    object.set("hostOnly"sv, cookie.host_only);
    object.set("partitionKey"sv, ""sv);
    return object;
}

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
    auto host = get_required_parameter<String>(message, "host"sv);
    if (!host.has_value())
        return;

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    auto requested_names = message.data.get_array("names"sv);

    Vector<HTTP::Cookie::Cookie> cookies;
    for (auto& cookie : devtools().delegate().cookies(tab->description())) {
        if (!cookie_matches_storage_host(cookie, *host))
            continue;

        if (requested_names.has_value()) {
            auto unique_key = cookie_unique_key(cookie);
            auto was_requested = any_of(requested_names->values(), [&](auto const& name) {
                return name.is_string() && name.as_string() == unique_key;
            });
            if (!was_requested)
                continue;
        }

        cookies.append(move(cookie));
    }

    quick_sort(cookies, [](auto const& left, auto const& right) {
        if (left.name != right.name)
            return left.name < right.name;
        if (left.domain != right.domain)
            return left.domain < right.domain;
        if (left.path.bytes().size() != right.path.bytes().size())
            return left.path.bytes().size() < right.path.bytes().size();
        return cookie_unique_key(left) < cookie_unique_key(right);
    });

    size_t offset = 0;
    size_t size = max_store_object_count;
    if (auto options = message.data.get_object("options"sv); options.has_value()) {
        offset = options->get_integer<size_t>("offset"sv).value_or(0);
        size = min(options->get_integer<size_t>("size"sv).value_or(max_store_object_count), max_store_object_count);
    }

    JsonArray data;
    if (offset < cookies.size()) {
        auto end = min(cookies.size(), offset + size);
        for (size_t i = offset; i < end; ++i)
            data.must_append(serialize_cookie(cookies[i]));
    } else {
        offset = cookies.size();
    }

    JsonObject response;
    response.set("offset"sv, offset);
    response.set("total"sv, cookies.size());
    response.set("data"sv, move(data));
    send_response(message, move(response));
}

}
