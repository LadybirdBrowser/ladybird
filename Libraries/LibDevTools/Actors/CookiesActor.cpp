/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <AK/Time.h>
#include <LibDevTools/Actors/CookiesActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

namespace DevTools {

static constexpr auto cookie_unique_key_separator = "{9d414cc5-8319-0a04-0586-c0a6ae01670a}"sv;
static constexpr auto max_store_object_count = 50uz;
static constexpr auto session_cookie_label = "Session"sv;
static constexpr auto default_cookie_value = "value"sv;

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

struct ParsedCookieUniqueKey {
    String name;
    String domain;
    String path;
};

static Optional<ParsedCookieUniqueKey> parse_cookie_unique_key(StringView unique_key)
{
    auto parts = unique_key.split_view(cookie_unique_key_separator, SplitBehavior::KeepEmpty);
    if (parts.size() < 3)
        return {};

    return ParsedCookieUniqueKey {
        MUST(String::from_utf8(parts[0])),
        MUST(String::from_utf8(parts[1])),
        MUST(String::from_utf8(parts[2])),
    };
}

static bool cookie_matches_unique_key(HTTP::Cookie::Cookie const& cookie, ParsedCookieUniqueKey const& unique_key)
{
    return cookie.name == unique_key.name
        && cookie.domain == unique_key.domain
        && cookie.path == unique_key.path;
}

static bool cookie_matches_requested_domain(HTTP::Cookie::Cookie const& cookie, Optional<String> const& domain)
{
    if (!domain.has_value())
        return true;
    return cookie.domain == *domain;
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

static Optional<bool> bool_from_devtools_value(JsonValue const& value)
{
    if (value.is_bool())
        return value.as_bool();
    if (!value.is_string())
        return {};

    auto string = value.as_string().bytes_as_string_view();
    if (string.equals_ignoring_ascii_case("true"sv))
        return true;
    if (string.equals_ignoring_ascii_case("false"sv))
        return false;
    return {};
}

static void set_session_cookie_expiry(HTTP::Cookie::Cookie& cookie)
{
    cookie.persistent = false;
    cookie.expiry_time = UnixDateTime::from_unix_time_parts(3000, 1, 1, 0, 0, 0, 0);
}

static Optional<String> set_cookie_expiry_from_devtools(HTTP::Cookie::Cookie& cookie, JsonValue const& json_value, StringView value)
{
    if (value.is_empty() || value == session_cookie_label) {
        set_session_cookie_expiry(cookie);
        return {};
    }

    if (json_value.is_number()) {
        cookie.persistent = true;
        cookie.expiry_time = UnixDateTime::from_milliseconds_since_epoch(json_value.get_number_with_precision_loss<i64>().release_value());
        return {};
    }

    if (auto milliseconds_since_epoch = value.to_number<i64>(); milliseconds_since_epoch.has_value()) {
        cookie.persistent = true;
        cookie.expiry_time = UnixDateTime::from_milliseconds_since_epoch(*milliseconds_since_epoch);
        return {};
    }

    auto expiry_time = HTTP::Cookie::parse_cookie_date(value);
    if (!expiry_time.has_value())
        return "Cookie expiry date is invalid"_string;

    cookie.persistent = true;
    cookie.expiry_time = *expiry_time;
    return {};
}

NonnullRefPtr<CookiesActor> CookiesActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new CookiesActor(devtools, move(name), move(tab)));
}

CookiesActor::CookiesActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
    if (auto tab = m_tab.strong_ref()) {
        devtools.delegate().listen_for_host_cookie_changes(
            tab->description(),
            weak_callback(*this, [](auto& self, Vector<HTTP::Cookie::Cookie> cookies) {
                self.on_cookies_changed(move(cookies));
            }));
    }
}

CookiesActor::~CookiesActor()
{
    if (auto tab = m_tab.strong_ref())
        devtools().delegate().stop_listening_for_host_cookie_changes(tab->description());
}

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
    traits.set("supportsAddItem"sv, true);
    traits.set("supportsRemoveAll"sv, true);
    traits.set("supportsRemoveAllSessionCookies"sv, true);
    traits.set("supportsRemoveItem"sv, true);

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

    if (message.type == "removeAllSessionCookies"sv) {
        remove_all_session_cookies(message);
        return;
    }

    send_unrecognized_packet_type_error(message);
}

void CookiesActor::get_fields(Message const& message)
{
    JsonArray fields;
    fields.must_append(define_storage_field("uniqueKey"sv, StorageFieldType::Private));
    fields.must_append(define_storage_field("name"sv, StorageFieldType::Mutable));
    fields.must_append(define_storage_field("value"sv, StorageFieldType::Mutable));
    fields.must_append(define_storage_field("host"sv, StorageFieldType::Mutable));
    fields.must_append(define_storage_field("path"sv, StorageFieldType::Mutable));
    fields.must_append(define_storage_field("expires"sv, StorageFieldType::Mutable));
    fields.must_append(define_storage_field("size"sv, StorageFieldType::Immutable));
    fields.must_append(define_storage_field("isHttpOnly"sv, StorageFieldType::Mutable));
    fields.must_append(define_storage_field("isSecure"sv, StorageFieldType::Mutable));
    fields.must_append(define_storage_field("sameSite"sv, StorageFieldType::Immutable));
    fields.must_append(define_storage_field("lastAccessed"sv, StorageFieldType::Immutable));
    fields.must_append(define_storage_field("creationTime"sv, StorageFieldType::Hidden));
    fields.must_append(define_storage_field("updateTime"sv, StorageFieldType::Hidden));
    fields.must_append(define_storage_field("hostOnly"sv, StorageFieldType::Hidden));
    fields.must_append(define_storage_field("partitionKey"sv, StorageFieldType::Immutable));

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

    if (!requested_names.has_value()) {
        HashTable<String> visible_keys;
        for (auto const& cookie : cookies)
            visible_keys.set(cookie_unique_key(cookie));
        m_visible_cookie_unique_keys.set(*host, move(visible_keys));
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

void CookiesActor::edit_item(Message const& message)
{
    auto data = get_required_parameter<JsonObject>(message, "data"sv);
    if (!data.has_value())
        return;

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    auto field = data->get_string("field"sv);
    if (!field.has_value()) {
        send_missing_parameter_error(message, "field"sv);
        return;
    }

    auto new_value_json = data->get("newValue"sv);
    if (!new_value_json.has_value()) {
        send_missing_parameter_error(message, "newValue"sv);
        return;
    }

    auto new_value = string_from_devtools_value(*new_value_json);
    if (!new_value.has_value()) {
        send_response(message, to_storage_operation_result("Missing cookie value"_string));
        return;
    }

    auto cookies = devtools().delegate().cookies(tab->description());
    auto find_cookie = [&](auto const& predicate) -> Optional<HTTP::Cookie::Cookie> {
        auto cookie = cookies.first_matching(predicate);
        if (!cookie.has_value())
            return {};
        return *cookie;
    };

    Optional<HTTP::Cookie::Cookie> old_cookie;
    auto items = data->get_object("items"sv);
    if (items.has_value()) {
        if (auto unique_key_string = items->get_string("uniqueKey"sv); unique_key_string.has_value()) {
            if (auto unique_key = parse_cookie_unique_key(unique_key_string->bytes_as_string_view()); unique_key.has_value())
                old_cookie = find_cookie([&](auto const& cookie) { return cookie_matches_unique_key(cookie, *unique_key); });
        }

        if (!old_cookie.has_value()) {
            Optional<String> old_value;
            if (auto old_value_json = data->get("oldValue"sv); old_value_json.has_value())
                old_value = string_from_devtools_value(*old_value_json);

            Optional<String> name;
            if (*field == "name"sv) {
                name = old_value;
            } else if (auto item_name = items->get_string("name"sv); item_name.has_value()) {
                name = *item_name;
            }

            Optional<String> domain;
            if (*field == "host"sv) {
                domain = old_value;
            } else if (auto item_host = items->get_string("host"sv); item_host.has_value()) {
                domain = *item_host;
            }

            Optional<String> path;
            if (*field == "path"sv) {
                path = old_value;
            } else if (auto item_path = items->get_string("path"sv); item_path.has_value()) {
                path = *item_path;
            }

            if (name.has_value() && domain.has_value() && path.has_value()) {
                old_cookie = find_cookie([&](auto const& cookie) {
                    return cookie.name == *name
                        && cookie.domain == *domain
                        && cookie.path == *path;
                });
            }
        }
    }

    if (!old_cookie.has_value()) {
        send_response(message, to_storage_operation_result(Optional<String> {}));
        return;
    }

    auto edited_cookie = *old_cookie;

    if (*field == "name"sv) {
        edited_cookie.name = new_value.release_value();
    } else if (*field == "value"sv) {
        edited_cookie.value = new_value.release_value();
    } else if (*field == "host"sv) {
        edited_cookie.domain = new_value.release_value();
    } else if (*field == "path"sv) {
        edited_cookie.path = new_value.release_value();
    } else if (*field == "expires"sv) {
        if (auto error_string = set_cookie_expiry_from_devtools(edited_cookie, *new_value_json, new_value->bytes_as_string_view()); error_string.has_value()) {
            send_response(message, to_storage_operation_result(move(error_string)));
            return;
        }
    } else if (*field == "isSecure"sv) {
        auto value = bool_from_devtools_value(*new_value_json);
        if (!value.has_value()) {
            send_response(message, to_storage_operation_result("Cookie secure flag must be true or false"_string));
            return;
        }
        edited_cookie.secure = *value;
    } else if (*field == "isHttpOnly"sv) {
        auto value = bool_from_devtools_value(*new_value_json);
        if (!value.has_value()) {
            send_response(message, to_storage_operation_result("Cookie HTTP-only flag must be true or false"_string));
            return;
        }
        edited_cookie.http_only = *value;
    } else if (*field == "sameSite"sv) {
        edited_cookie.same_site = HTTP::Cookie::same_site_from_string(new_value->bytes_as_string_view());
    } else {
        send_response(message, to_storage_operation_result(Optional<String> {}));
        return;
    }

    auto result = devtools().delegate().set_cookie(tab->description(), old_cookie, move(edited_cookie));
    send_response(message, to_storage_operation_result(move(result)));
}

void CookiesActor::add_item(Message const& message)
{
    auto guid = get_required_parameter<String>(message, "guid"sv);
    if (!guid.has_value())
        return;

    auto storage_host = message.data.get_string("host"sv)
                            .value_or_lazy_evaluated_optional([this] { return host(); });
    if (!storage_host.has_value()) {
        send_response(message, to_storage_operation_result("No storage host is available for this page"_string));
        return;
    }

    auto storage_url = URL::Parser::basic_parse(*storage_host);
    auto host_name = storage_host_name(*storage_host);
    if (!storage_url.has_value() || !host_name.has_value()) {
        send_response(message, to_storage_operation_result("Cannot add a cookie for this storage host"_string));
        return;
    }

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    auto now = UnixDateTime::now();

    HTTP::Cookie::Cookie cookie;
    cookie.name = move(*guid);
    cookie.value = MUST(String::from_utf8(default_cookie_value));
    cookie.same_site = HTTP::Cookie::SameSite::Lax;
    cookie.creation_time = now;
    cookie.last_access_time = now;
    set_session_cookie_expiry(cookie);
    cookie.domain = move(*host_name);
    cookie.path = "/"_string;
    cookie.secure = storage_url->scheme() == "https"sv;
    cookie.host_only = true;

    auto result = devtools().delegate().set_cookie(tab->description(), {}, move(cookie));
    send_response(message, to_storage_operation_result(result));
}

void CookiesActor::remove_item(Message const& message)
{
    auto host = get_required_parameter<String>(message, "host"sv);
    if (!host.has_value())
        return;

    auto name = get_required_parameter<String>(message, "name"sv);
    if (!name.has_value())
        return;

    auto unique_key = parse_cookie_unique_key(name->bytes_as_string_view());
    if (!unique_key.has_value()) {
        send_response(message, {});
        return;
    }

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    Vector<HTTP::Cookie::Cookie> cookies_to_delete;
    for (auto& cookie : devtools().delegate().cookies(tab->description())) {
        if (!cookie_matches_storage_host(cookie, *host))
            continue;
        if (!cookie_matches_unique_key(cookie, *unique_key))
            continue;
        cookies_to_delete.append(move(cookie));
    }

    devtools().delegate().delete_cookies(tab->description(), move(cookies_to_delete));
    send_response(message, {});
}

void CookiesActor::remove_all(Message const& message)
{
    auto host = get_required_parameter<String>(message, "host"sv);
    if (!host.has_value())
        return;

    Optional<String> domain;
    if (auto requested_domain = message.data.get_string("domain"sv); requested_domain.has_value())
        domain = *requested_domain;

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    Vector<HTTP::Cookie::Cookie> cookies_to_delete;
    for (auto& cookie : devtools().delegate().cookies(tab->description())) {
        if (!cookie_matches_storage_host(cookie, *host))
            continue;
        if (!cookie_matches_requested_domain(cookie, domain))
            continue;
        cookies_to_delete.append(move(cookie));
    }

    devtools().delegate().delete_cookies(tab->description(), move(cookies_to_delete));
    send_response(message, {});
}

void CookiesActor::remove_all_session_cookies(Message const& message)
{
    auto host = get_required_parameter<String>(message, "host"sv);
    if (!host.has_value())
        return;

    Optional<String> domain;
    if (auto requested_domain = message.data.get_string("domain"sv); requested_domain.has_value())
        domain = *requested_domain;

    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    Vector<HTTP::Cookie::Cookie> cookies_to_delete;
    for (auto& cookie : devtools().delegate().cookies(tab->description())) {
        if (!cookie_matches_storage_host(cookie, *host))
            continue;
        if (!cookie_matches_requested_domain(cookie, domain))
            continue;
        if (cookie.persistent)
            continue;
        cookies_to_delete.append(move(cookie));
    }

    devtools().delegate().delete_cookies(tab->description(), move(cookies_to_delete));
    send_response(message, {});
}

HashTable<String> CookiesActor::visible_cookie_unique_keys(String const& host) const
{
    HashTable<String> visible_keys;
    auto tab = m_tab.strong_ref();
    if (!tab)
        return visible_keys;

    for (auto& cookie : devtools().delegate().cookies(tab->description())) {
        if (cookie_matches_storage_host(cookie, host))
            visible_keys.set(cookie_unique_key(cookie));
    }
    return visible_keys;
}

void CookiesActor::send_cookie_store_update(String const& host, JsonArray added, JsonArray changed, JsonArray deleted)
{
    JsonObject update;

    auto append_update = [&](StringView update_type, JsonArray keys) {
        if (keys.is_empty())
            return;

        JsonObject hosts;
        hosts.set(host, move(keys));

        JsonObject cookies;
        cookies.set("cookies"sv, move(hosts));

        update.set(update_type, move(cookies));
    };

    append_update("added"sv, move(added));
    append_update("changed"sv, move(changed));
    append_update("deleted"sv, move(deleted));

    JsonObject message;
    message.set("type"sv, "storesUpdate"sv);
    message.set("data"sv, move(update));
    send_message(move(message));
}

void CookiesActor::on_cookies_changed(Vector<HTTP::Cookie::Cookie> changed_cookies)
{
    auto storage_host = host();
    if (!storage_host.has_value())
        return;

    HashTable<String> old_visible_keys;
    if (auto it = m_visible_cookie_unique_keys.find(*storage_host); it != m_visible_cookie_unique_keys.end())
        old_visible_keys = it->value;

    auto current_visible_keys = visible_cookie_unique_keys(*storage_host);

    JsonArray added;
    JsonArray changed;
    JsonArray deleted;

    for (auto const& cookie : changed_cookies) {
        if (!cookie_matches_storage_host(cookie, *storage_host))
            continue;

        auto unique_key = cookie_unique_key(cookie);
        auto was_visible = old_visible_keys.contains(unique_key);
        auto is_visible = current_visible_keys.contains(unique_key);

        if (is_visible && was_visible)
            changed.must_append(unique_key);
        else if (is_visible)
            added.must_append(unique_key);
        else if (was_visible)
            deleted.must_append(unique_key);
    }

    m_visible_cookie_unique_keys.set(*storage_host, move(current_visible_keys));

    if (added.is_empty() && changed.is_empty() && deleted.is_empty())
        return;

    send_cookie_store_update(*storage_host, move(added), move(changed), move(deleted));
}

}
