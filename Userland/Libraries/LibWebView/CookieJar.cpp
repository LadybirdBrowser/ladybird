/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@gmta.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/URL.h>

namespace WebView {

static constexpr auto DATABASE_SYNCHRONIZATION_TIMER = AK::Duration::from_seconds(30);

ErrorOr<NonnullOwnPtr<CookieJar>> CookieJar::create(Database& database)
{
    Statements statements {};

    auto create_table = TRY(database.prepare_statement(MUST(String::formatted(R"#(
        CREATE TABLE IF NOT EXISTS Cookies (
            name TEXT,
            value TEXT,
            same_site INTEGER CHECK (same_site >= 0 AND same_site <= {}),
            creation_time INTEGER,
            last_access_time INTEGER,
            expiry_time INTEGER,
            domain TEXT,
            path TEXT,
            secure BOOLEAN,
            http_only BOOLEAN,
            host_only BOOLEAN,
            persistent BOOLEAN,
            PRIMARY KEY(name, domain, path)
        );)#",
        to_underlying(Web::Cookie::SameSite::Lax)))));
    database.execute_statement(create_table, {});

    statements.insert_cookie = TRY(database.prepare_statement("INSERT OR REPLACE INTO Cookies VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"sv));
    statements.expire_cookie = TRY(database.prepare_statement("DELETE FROM Cookies WHERE (expiry_time < ?);"sv));
    statements.select_all_cookies = TRY(database.prepare_statement("SELECT * FROM Cookies;"sv));

    return adopt_own(*new CookieJar { PersistedStorage { database, statements } });
}

NonnullOwnPtr<CookieJar> CookieJar::create()
{
    return adopt_own(*new CookieJar { OptionalNone {} });
}

CookieJar::CookieJar(Optional<PersistedStorage> persisted_storage)
    : m_persisted_storage(move(persisted_storage))
{
    if (!m_persisted_storage.has_value())
        return;

    // FIXME: Make cookie retrieval lazy so we don't need to retrieve all cookies up front.
    auto cookies = m_persisted_storage->select_all_cookies();
    m_transient_storage.set_cookies(move(cookies));

    m_persisted_storage->synchronization_timer = Core::Timer::create_repeating(
        static_cast<int>(DATABASE_SYNCHRONIZATION_TIMER.to_milliseconds()),
        [this]() {
            auto now = m_transient_storage.purge_expired_cookies();
            m_persisted_storage->database.execute_statement(m_persisted_storage->statements.expire_cookie, {}, now);

            for (auto const& it : m_transient_storage.take_dirty_cookies())
                m_persisted_storage->insert_cookie(it.value);
        });
    m_persisted_storage->synchronization_timer->start();
}

CookieJar::~CookieJar()
{
    if (!m_persisted_storage.has_value())
        return;

    m_persisted_storage->synchronization_timer->stop();
    m_persisted_storage->synchronization_timer->on_timeout();
}

String CookieJar::get_cookie(const URL::URL& url, Web::Cookie::Source source)
{
    m_transient_storage.purge_expired_cookies();

    auto domain = canonicalize_domain(url);
    if (!domain.has_value())
        return {};

    auto cookie_list = get_matching_cookies(url, domain.value(), source);
    StringBuilder builder;

    for (auto const& cookie : cookie_list) {
        // If there is an unprocessed cookie in the cookie-list, output the characters %x3B and %x20 ("; ")
        if (!builder.is_empty())
            builder.append("; "sv);

        // Output the cookie's name, the %x3D ("=") character, and the cookie's value.
        builder.appendff("{}={}", cookie.name, cookie.value);
    }

    return MUST(builder.to_string());
}

void CookieJar::set_cookie(const URL::URL& url, Web::Cookie::ParsedCookie const& parsed_cookie, Web::Cookie::Source source)
{
    auto domain = canonicalize_domain(url);
    if (!domain.has_value())
        return;

    store_cookie(parsed_cookie, url, domain.release_value(), source);
}

// This is based on https://www.rfc-editor.org/rfc/rfc6265#section-5.3 as store_cookie() below
// however the whole ParsedCookie->Cookie conversion is skipped.
void CookieJar::update_cookie(Web::Cookie::Cookie cookie)
{
    CookieStorageKey key { cookie.name, cookie.domain, cookie.path };

    // 11. If the cookie store contains a cookie with the same name, domain, and path as the newly created cookie:
    if (auto old_cookie = m_transient_storage.get_cookie(key); old_cookie.has_value()) {
        // Update the creation-time of the newly created cookie to match the creation-time of the old-cookie.
        cookie.creation_time = old_cookie->creation_time;

        // Remove the old-cookie from the cookie store.
        // NOTE: Rather than deleting then re-inserting this cookie, we update it in-place.
    }

    // 12. Insert the newly created cookie into the cookie store.
    m_transient_storage.set_cookie(move(key), move(cookie));

    m_transient_storage.purge_expired_cookies();
}

void CookieJar::dump_cookies()
{
    StringBuilder builder;

    m_transient_storage.for_each_cookie([&](auto const& cookie) {
        static constexpr auto key_color = "\033[34;1m"sv;
        static constexpr auto attribute_color = "\033[33m"sv;
        static constexpr auto no_color = "\033[0m"sv;

        builder.appendff("{}{}{} - ", key_color, cookie.name, no_color);
        builder.appendff("{}{}{} - ", key_color, cookie.domain, no_color);
        builder.appendff("{}{}{}\n", key_color, cookie.path, no_color);

        builder.appendff("\t{}Value{} = {}\n", attribute_color, no_color, cookie.value);
        builder.appendff("\t{}CreationTime{} = {}\n", attribute_color, no_color, cookie.creation_time_to_string());
        builder.appendff("\t{}LastAccessTime{} = {}\n", attribute_color, no_color, cookie.last_access_time_to_string());
        builder.appendff("\t{}ExpiryTime{} = {}\n", attribute_color, no_color, cookie.expiry_time_to_string());
        builder.appendff("\t{}Secure{} = {:s}\n", attribute_color, no_color, cookie.secure);
        builder.appendff("\t{}HttpOnly{} = {:s}\n", attribute_color, no_color, cookie.http_only);
        builder.appendff("\t{}HostOnly{} = {:s}\n", attribute_color, no_color, cookie.host_only);
        builder.appendff("\t{}Persistent{} = {:s}\n", attribute_color, no_color, cookie.persistent);
        builder.appendff("\t{}SameSite{} = {:s}\n", attribute_color, no_color, Web::Cookie::same_site_to_string(cookie.same_site));
    });

    dbgln("{} cookies stored\n{}", m_transient_storage.size(), builder.string_view());
}

Vector<Web::Cookie::Cookie> CookieJar::get_all_cookies()
{
    Vector<Web::Cookie::Cookie> cookies;
    cookies.ensure_capacity(m_transient_storage.size());

    m_transient_storage.for_each_cookie([&](auto const& cookie) {
        cookies.unchecked_append(cookie);
    });

    return cookies;
}

// https://w3c.github.io/webdriver/#dfn-associated-cookies
Vector<Web::Cookie::Cookie> CookieJar::get_all_cookies(URL::URL const& url)
{
    auto domain = canonicalize_domain(url);
    if (!domain.has_value())
        return {};

    return get_matching_cookies(url, domain.value(), Web::Cookie::Source::Http, MatchingCookiesSpecMode::WebDriver);
}

Optional<Web::Cookie::Cookie> CookieJar::get_named_cookie(URL::URL const& url, StringView name)
{
    auto domain = canonicalize_domain(url);
    if (!domain.has_value())
        return {};

    auto cookie_list = get_matching_cookies(url, domain.value(), Web::Cookie::Source::Http, MatchingCookiesSpecMode::WebDriver);

    for (auto const& cookie : cookie_list) {
        if (cookie.name == name)
            return cookie;
    }

    return {};
}

Optional<String> CookieJar::canonicalize_domain(const URL::URL& url)
{
    // https://tools.ietf.org/html/rfc6265#section-5.1.2
    if (!url.is_valid())
        return {};

    // FIXME: Implement RFC 5890 to "Convert each label that is not a Non-Reserved LDH (NR-LDH) label to an A-label".
    if (url.host().has<Empty>())
        return {};

    return MUST(MUST(url.serialized_host()).to_lowercase());
}

bool CookieJar::domain_matches(StringView string, StringView domain_string)
{
    // https://tools.ietf.org/html/rfc6265#section-5.1.3

    // A string domain-matches a given domain string if at least one of the following conditions hold:

    // The domain string and the string are identical.
    if (string == domain_string)
        return true;

    // All of the following conditions hold:
    //   - The domain string is a suffix of the string.
    //   - The last character of the string that is not included in the domain string is a %x2E (".") character.
    //   - The string is a host name (i.e., not an IP address).
    if (!string.ends_with(domain_string))
        return false;
    if (string[string.length() - domain_string.length() - 1] != '.')
        return false;
    if (AK::IPv4Address::from_string(string).has_value())
        return false;

    return true;
}

bool CookieJar::path_matches(StringView request_path, StringView cookie_path)
{
    // https://tools.ietf.org/html/rfc6265#section-5.1.4

    // A request-path path-matches a given cookie-path if at least one of the following conditions holds:

    // The cookie-path and the request-path are identical.
    if (request_path == cookie_path)
        return true;

    if (request_path.starts_with(cookie_path)) {
        // The cookie-path is a prefix of the request-path, and the last character of the cookie-path is %x2F ("/").
        if (cookie_path.ends_with('/'))
            return true;

        // The cookie-path is a prefix of the request-path, and the first character of the request-path that is not included in the cookie-path is a %x2F ("/") character.
        if (request_path[cookie_path.length()] == '/')
            return true;
    }

    return false;
}

String CookieJar::default_path(const URL::URL& url)
{
    // https://tools.ietf.org/html/rfc6265#section-5.1.4

    // 1. Let uri-path be the path portion of the request-uri if such a portion exists (and empty otherwise).
    auto uri_path = url.serialize_path();

    // 2. If the uri-path is empty or if the first character of the uri-path is not a %x2F ("/") character, output %x2F ("/") and skip the remaining steps.
    if (uri_path.is_empty() || (uri_path[0] != '/'))
        return "/"_string;

    StringView uri_path_view = uri_path;
    size_t last_separator = uri_path_view.find_last('/').value();

    // 3. If the uri-path contains no more than one %x2F ("/") character, output %x2F ("/") and skip the remaining step.
    if (last_separator == 0)
        return "/"_string;

    // 4. Output the characters of the uri-path from the first character up to, but not including, the right-most %x2F ("/").
    return MUST(String::from_utf8(uri_path.substring_view(0, last_separator)));
}

void CookieJar::store_cookie(Web::Cookie::ParsedCookie const& parsed_cookie, const URL::URL& url, String canonicalized_domain, Web::Cookie::Source source)
{
    // https://tools.ietf.org/html/rfc6265#section-5.3

    // 2. Create a new cookie with name cookie-name, value cookie-value. Set the creation-time and the last-access-time to the current date and time.
    Web::Cookie::Cookie cookie { parsed_cookie.name, parsed_cookie.value, parsed_cookie.same_site_attribute };
    cookie.creation_time = UnixDateTime::now();
    cookie.last_access_time = cookie.creation_time;

    if (parsed_cookie.expiry_time_from_max_age_attribute.has_value()) {
        // 3. If the cookie-attribute-list contains an attribute with an attribute-name of "Max-Age": Set the cookie's persistent-flag to true.
        // Set the cookie's expiry-time to attribute-value of the last attribute in the cookie-attribute-list with an attribute-name of "Max-Age".
        cookie.persistent = true;
        cookie.expiry_time = parsed_cookie.expiry_time_from_max_age_attribute.value();
    } else if (parsed_cookie.expiry_time_from_expires_attribute.has_value()) {
        // If the cookie-attribute-list contains an attribute with an attribute-name of "Expires": Set the cookie's persistent-flag to true.
        // Set the cookie's expiry-time to attribute-value of the last attribute in the cookie-attribute-list with an attribute-name of "Expires".
        cookie.persistent = true;
        cookie.expiry_time = parsed_cookie.expiry_time_from_expires_attribute.value();
    } else {
        // Set the cookie's persistent-flag to false. Set the cookie's expiry-time to the latest representable date.
        cookie.persistent = false;
        cookie.expiry_time = UnixDateTime::from_unix_time_parts(3000, 1, 1, 0, 0, 0, 0);
    }

    // 4. If the cookie-attribute-list contains an attribute with an attribute-name of "Domain":
    if (parsed_cookie.domain.has_value()) {
        // Let the domain-attribute be the attribute-value of the last attribute in the cookie-attribute-list with an attribute-name of "Domain".
        cookie.domain = parsed_cookie.domain.value();
    }

    // 5. If the user agent is configured to reject "public suffixes" and the domain-attribute is a public suffix:
    if (is_public_suffix(cookie.domain)) {
        // If the domain-attribute is identical to the canonicalized request-host:
        if (cookie.domain == canonicalized_domain) {
            // Let the domain-attribute be the empty string.
            cookie.domain = String {};
        }
        // Otherwise:
        else {
            // Ignore the cookie entirely and abort these steps.
            return;
        }
    }

    // 6. If the domain-attribute is non-empty:
    if (!cookie.domain.is_empty()) {
        // If the canonicalized request-host does not domain-match the domain-attribute: Ignore the cookie entirely and abort these steps.
        if (!domain_matches(canonicalized_domain, cookie.domain))
            return;

        // Set the cookie's host-only-flag to false. Set the cookie's domain to the domain-attribute.
        cookie.host_only = false;
    } else {
        // Set the cookie's host-only-flag to true. Set the cookie's domain to the canonicalized request-host.
        cookie.host_only = true;
        cookie.domain = move(canonicalized_domain);
    }

    // 7. If the cookie-attribute-list contains an attribute with an attribute-name of "Path":
    if (parsed_cookie.path.has_value()) {
        // Set the cookie's path to attribute-value of the last attribute in the cookie-attribute-list with an attribute-name of "Path".
        cookie.path = parsed_cookie.path.value();
    } else {
        cookie.path = default_path(url);
    }

    // 8. If the cookie-attribute-list contains an attribute with an attribute-name of "Secure", set the cookie's secure-only-flag to true.
    cookie.secure = parsed_cookie.secure_attribute_present;

    // 9. If the cookie-attribute-list contains an attribute with an attribute-name of "HttpOnly", set the cookie's http-only-flag to false.
    cookie.http_only = parsed_cookie.http_only_attribute_present;

    // 10. If the cookie was received from a "non-HTTP" API and the cookie's http-only-flag is set, abort these steps and ignore the cookie entirely.
    if (source != Web::Cookie::Source::Http && cookie.http_only)
        return;

    CookieStorageKey key { cookie.name, cookie.domain, cookie.path };

    // 11. If the cookie store contains a cookie with the same name, domain, and path as the newly created cookie:
    if (auto const& old_cookie = m_transient_storage.get_cookie(key); old_cookie.has_value()) {
        // If the newly created cookie was received from a "non-HTTP" API and the old-cookie's http-only-flag is set, abort these
        // steps and ignore the newly created cookie entirely.
        if (source != Web::Cookie::Source::Http && old_cookie->http_only)
            return;

        // Update the creation-time of the newly created cookie to match the creation-time of the old-cookie.
        cookie.creation_time = old_cookie->creation_time;

        // Remove the old-cookie from the cookie store.
        // NOTE: Rather than deleting then re-inserting this cookie, we update it in-place.
    }

    // 12. Insert the newly created cookie into the cookie store.
    m_transient_storage.set_cookie(move(key), move(cookie));

    m_transient_storage.purge_expired_cookies();
}

Vector<Web::Cookie::Cookie> CookieJar::get_matching_cookies(const URL::URL& url, StringView canonicalized_domain, Web::Cookie::Source source, MatchingCookiesSpecMode mode)
{
    // https://tools.ietf.org/html/rfc6265#section-5.4
    auto now = UnixDateTime::now();

    // 1. Let cookie-list be the set of cookies from the cookie store that meets all of the following requirements:
    Vector<Web::Cookie::Cookie> cookie_list;

    m_transient_storage.for_each_cookie([&](auto& cookie) {
        // Either: The cookie's host-only-flag is true and the canonicalized request-host is identical to the cookie's domain.
        // Or: The cookie's host-only-flag is false and the canonicalized request-host domain-matches the cookie's domain.
        bool is_host_only_and_has_identical_domain = cookie.host_only && (canonicalized_domain == cookie.domain);
        bool is_not_host_only_and_domain_matches = !cookie.host_only && domain_matches(canonicalized_domain, cookie.domain);
        if (!is_host_only_and_has_identical_domain && !is_not_host_only_and_domain_matches)
            return;

        // The request-uri's path path-matches the cookie's path.
        if (!path_matches(url.serialize_path(), cookie.path))
            return;

        // If the cookie's secure-only-flag is true, then the request-uri's scheme must denote a "secure" protocol.
        if (cookie.secure && (url.scheme() != "https"))
            return;

        // If the cookie's http-only-flag is true, then exclude the cookie if the cookie-string is being generated for a "non-HTTP" API.
        if (cookie.http_only && (source != Web::Cookie::Source::Http))
            return;

        // NOTE: The WebDriver spec expects only step 1 above to be executed to match cookies.
        if (mode == MatchingCookiesSpecMode::WebDriver) {
            cookie_list.append(cookie);
            return;
        }

        // 3. Update the last-access-time of each cookie in the cookie-list to the current date and time.
        // NOTE: We do this first so that both our internal storage and cookie-list are updated.
        cookie.last_access_time = now;

        // 2.  The user agent SHOULD sort the cookie-list in the following order:
        //   - Cookies with longer paths are listed before cookies with shorter paths.
        //   - Among cookies that have equal-length path fields, cookies with earlier creation-times are listed before cookies with later creation-times.
        auto cookie_path_length = cookie.path.bytes().size();
        auto cookie_creation_time = cookie.creation_time;

        cookie_list.insert_before_matching(cookie, [cookie_path_length, cookie_creation_time](auto const& entry) {
            if (cookie_path_length > entry.path.bytes().size()) {
                return true;
            }
            if (cookie_path_length == entry.path.bytes().size()) {
                if (cookie_creation_time < entry.creation_time)
                    return true;
            }
            return false;
        });
    });

    if (mode != MatchingCookiesSpecMode::WebDriver)
        m_transient_storage.purge_expired_cookies();

    return cookie_list;
}

void CookieJar::TransientStorage::set_cookies(Cookies cookies)
{
    m_cookies = move(cookies);
    purge_expired_cookies();
}

void CookieJar::TransientStorage::set_cookie(CookieStorageKey key, Web::Cookie::Cookie cookie)
{
    m_cookies.set(key, cookie);
    m_dirty_cookies.set(move(key), move(cookie));
}

Optional<Web::Cookie::Cookie> CookieJar::TransientStorage::get_cookie(CookieStorageKey const& key)
{
    return m_cookies.get(key);
}

UnixDateTime CookieJar::TransientStorage::purge_expired_cookies()
{
    auto now = UnixDateTime::now();
    auto is_expired = [&](auto const&, auto const& cookie) { return cookie.expiry_time < now; };

    m_cookies.remove_all_matching(is_expired);
    m_dirty_cookies.remove_all_matching(is_expired);

    return now;
}

void CookieJar::PersistedStorage::insert_cookie(Web::Cookie::Cookie const& cookie)
{
    database.execute_statement(
        statements.insert_cookie,
        {},
        cookie.name,
        cookie.value,
        to_underlying(cookie.same_site),
        cookie.creation_time,
        cookie.last_access_time,
        cookie.expiry_time,
        cookie.domain,
        cookie.path,
        cookie.secure,
        cookie.http_only,
        cookie.host_only,
        cookie.persistent);
}

static Web::Cookie::Cookie parse_cookie(Database& database, Database::StatementID statement_id)
{
    int column = 0;
    auto convert_text = [&](auto& field) { field = database.result_column<String>(statement_id, column++); };
    auto convert_bool = [&](auto& field) { field = database.result_column<bool>(statement_id, column++); };
    auto convert_time = [&](auto& field) { field = database.result_column<UnixDateTime>(statement_id, column++); };

    auto convert_same_site = [&](auto& field) {
        auto same_site = database.result_column<UnderlyingType<Web::Cookie::SameSite>>(statement_id, column++);
        field = static_cast<Web::Cookie::SameSite>(same_site);
    };

    Web::Cookie::Cookie cookie;
    convert_text(cookie.name);
    convert_text(cookie.value);
    convert_same_site(cookie.same_site);
    convert_time(cookie.creation_time);
    convert_time(cookie.last_access_time);
    convert_time(cookie.expiry_time);
    convert_text(cookie.domain);
    convert_text(cookie.path);
    convert_bool(cookie.secure);
    convert_bool(cookie.http_only);
    convert_bool(cookie.host_only);
    convert_bool(cookie.persistent);

    return cookie;
}

CookieJar::TransientStorage::Cookies CookieJar::PersistedStorage::select_all_cookies()
{
    HashMap<CookieStorageKey, Web::Cookie::Cookie> cookies;

    database.execute_statement(
        statements.select_all_cookies,
        [&](auto statement_id) {
            auto cookie = parse_cookie(database, statement_id);

            CookieStorageKey key { cookie.name, cookie.domain, cookie.path };
            cookies.set(move(key), move(cookie));
        });

    return cookies;
}

}
