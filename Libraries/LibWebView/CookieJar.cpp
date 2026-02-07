/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibURL/URL.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

// For updates to the "Cookies: HTTP State Management Mechanism" RFC, see:
// https://datatracker.ietf.org/doc/draft-ietf-httpbis-rfc6265bis/history/

static constexpr auto DATABASE_SYNCHRONIZATION_TIMER = AK::Duration::from_seconds(30);

ErrorOr<NonnullOwnPtr<CookieJar>> CookieJar::create(Database::Database& database)
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
        to_underlying(HTTP::Cookie::SameSite::Lax)))));
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
            for (auto const& it : m_transient_storage.take_dirty_cookies())
                m_persisted_storage->insert_cookie(it.value);

            auto now = m_transient_storage.purge_expired_cookies();
            m_persisted_storage->database.execute_statement(m_persisted_storage->statements.expire_cookie, {}, now);
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

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-22#section-5.8.3
String CookieJar::get_cookie(URL::URL const& url, HTTP::Cookie::Source source)
{
    m_transient_storage.purge_expired_cookies();

    auto cookie_list = get_matching_cookies(url, source);

    // 6. Serialize the cookie-list into a cookie-string by processing each cookie in the cookie-list in order:
    StringBuilder builder;

    for (auto const& cookie : cookie_list) {
        if (!builder.is_empty())
            builder.append("; "sv);

        // 1. If the cookies' name is not empty, output the cookie's name followed by the %x3D ("=") character.
        if (!cookie.name.is_empty())
            builder.appendff("{}=", cookie.name);

        // 2. If the cookies' value is not empty, output the cookie's value.
        if (!cookie.value.is_empty())
            builder.append(cookie.value);

        // 3. If the cookie was not the last cookie in the cookie-list, output the characters %x3B and %x20 ("; ").
    }

    return MUST(builder.to_string());
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-22#section-5.7
void CookieJar::set_cookie(URL::URL const& url, HTTP::Cookie::ParsedCookie const& parsed_cookie, HTTP::Cookie::Source source)
{
    // 1. A user agent MAY ignore a received cookie in its entirety. See Section 5.3.

    // 2. If cookie-name is empty and cookie-value is empty, abort this algorithm and ignore the cookie entirely.
    if (parsed_cookie.name.is_empty() && parsed_cookie.value.is_empty())
        return;

    // 3. If the cookie-name or the cookie-value contains a %x00-08 / %x0A-1F / %x7F character (CTL characters excluding
    //    HTAB), abort this algorithm and ignore the cookie entirely.
    if (HTTP::Cookie::cookie_contains_invalid_control_character(parsed_cookie.name))
        return;
    if (HTTP::Cookie::cookie_contains_invalid_control_character(parsed_cookie.value))
        return;

    // 4. If the sum of the lengths of cookie-name and cookie-value is more than 4096 octets, abort this algorithm and
    //    ignore the cookie entirely.
    if (parsed_cookie.name.byte_count() + parsed_cookie.value.byte_count() > 4096)
        return;

    // 5. Create a new cookie with name cookie-name, value cookie-value. Set the creation-time and the last-access-time
    //    to the current date and time.
    HTTP::Cookie::Cookie cookie { parsed_cookie.name, parsed_cookie.value };
    cookie.creation_time = UnixDateTime::now();
    cookie.last_access_time = cookie.creation_time;

    // 6. If the cookie-attribute-list contains an attribute with an attribute-name of "Max-Age":
    if (parsed_cookie.expiry_time_from_max_age_attribute.has_value()) {
        // 1. Set the cookie's persistent-flag to true.
        cookie.persistent = true;

        // 2. Set the cookie's expiry-time to attribute-value of the last attribute in the cookie-attribute-list with
        //    an attribute-name of "Max-Age".
        cookie.expiry_time = parsed_cookie.expiry_time_from_max_age_attribute.value();
    }
    // Otherwise, if the cookie-attribute-list contains an attribute with an attribute-name of "Expires" (and does not
    // contain an attribute with an attribute-name of "Max-Age"):
    else if (parsed_cookie.expiry_time_from_expires_attribute.has_value()) {
        // 1. Set the cookie's persistent-flag to true.
        cookie.persistent = true;

        // 2. Set the cookie's expiry-time to attribute-value of the last attribute in the cookie-attribute-list with
        //    an attribute-name of "Expires".
        cookie.expiry_time = parsed_cookie.expiry_time_from_expires_attribute.value();
    }
    // Otherwise:
    else {
        // 1. Set the cookie's persistent-flag to false.
        cookie.persistent = false;

        // 2. Set the cookie's expiry-time to the latest representable date.
        cookie.expiry_time = UnixDateTime::from_unix_time_parts(3000, 1, 1, 0, 0, 0, 0);
    }

    String domain_attribute;

    // 7. If the cookie-attribute-list contains an attribute with an attribute-name of "Domain":
    if (parsed_cookie.domain.has_value()) {
        // 1. Let the domain-attribute be the attribute-value of the last attribute in the cookie-attribute-list with
        //    both an attribute-name of "Domain" and an attribute-value whose length is no more than 1024 octets. (Note
        //    that a leading %x2E ("."), if present, is ignored even though that character is not permitted.)
        if (parsed_cookie.domain->byte_count() <= 1024)
            domain_attribute = parsed_cookie.domain.value();
    }
    // Otherwise:
    else {
        // 1. Let the domain-attribute be the empty string.
    }

    // 8. If the domain-attribute contains a character that is not in CHAR, abort this algorithm and ignore the cookie
    //    entirely.
    if (!domain_attribute.is_ascii())
        return;

    auto request_host_canonical = HTTP::Cookie::canonicalize_domain(url);
    if (!request_host_canonical.has_value())
        return;

    // 9. If the user agent is configured to reject "public suffixes" and the domain-attribute is a public suffix:
    if (URL::is_public_suffix(domain_attribute)) {
        // 1. Let request-host-canonical be the canonicalized request-host.
        // 2. If request-host fails to be canonicalized then abort this algorithm and ignore the cookie entirely.

        // 3. If the domain-attribute is identical to the request-host-canonical:
        if (domain_attribute == *request_host_canonical) {
            // 1. Let the domain-attribute be the empty string.
            domain_attribute = String {};
        }
        // Otherwise:
        else {
            // 1. Abort this algorithm and ignore the cookie entirely.
            return;
        }
    }

    // 10. If the domain-attribute is non-empty:
    if (!domain_attribute.is_empty()) {
        // 1. If request-host-canonical does not domain-match (see Section 5.1.3) the domain-attribute:
        if (!HTTP::Cookie::domain_matches(*request_host_canonical, domain_attribute)) {
            // 1. Abort this algorithm and ignore the cookie entirely.
            return;
        }
        // Otherwise:
        else {
            // 1. Set the cookie's host-only-flag to false.
            cookie.host_only = false;

            // 2. Set the cookie's domain to the domain-attribute.
            cookie.domain = move(domain_attribute);
        }
    }
    // Otherwise:
    else {
        // 1. Set the cookie's host-only-flag to true.
        cookie.host_only = true;

        // 2. Set the cookie's domain to request-host-canonical.
        cookie.domain = request_host_canonical.release_value();
    }

    // 11. If the cookie-attribute-list contains an attribute with an attribute-name of "Path", set the cookie's path to
    //     attribute-value of the last attribute in the cookie-attribute-list with both an attribute-name of "Path" and
    //     an attribute-value whose length is no more than 1024 octets. Otherwise, set the cookie's path to the
    //     default-path of the request-uri.
    if (parsed_cookie.path.has_value()) {
        if (parsed_cookie.path->byte_count() <= 1024)
            cookie.path = parsed_cookie.path.value();
    } else {
        cookie.path = HTTP::Cookie::default_path(url);
    }

    // 12. If the cookie-attribute-list contains an attribute with an attribute-name of "Secure", set the cookie's
    //     secure-only-flag to true. Otherwise, set the cookie's secure-only-flag to false.
    cookie.secure = parsed_cookie.secure_attribute_present;

    // 13. If the request-uri does not denote a "secure" connection (as defined by the user agent), and the cookie's
    //     secure-only-flag is true, then abort these steps and ignore the cookie entirely.
    if (cookie.secure && url.scheme() != "https"sv)
        return;

    // 14. If the cookie-attribute-list contains an attribute with an attribute-name of "HttpOnly", set the cookie's
    //     http-only-flag to true. Otherwise, set the cookie's http-only-flag to false.
    cookie.http_only = parsed_cookie.http_only_attribute_present;

    // 15. If the cookie was received from a "non-HTTP" API and the cookie's http-only-flag is true, abort this
    //     algorithm and ignore the cookie entirely.
    if (source == HTTP::Cookie::Source::NonHttp && cookie.http_only)
        return;

    // 16. If the cookie's secure-only-flag is false, and the request-uri does not denote a "secure" connection, then
    //     abort this algorithm and ignore the cookie entirely if the cookie store contains one or more cookies that
    //     meet all of the following criteria:
    if (!cookie.secure && url.scheme() != "https"sv) {
        auto ignore_cookie = false;

        m_transient_storage.for_each_cookie([&](HTTP::Cookie::Cookie const& old_cookie) {
            // 1. Their name matches the name of the newly-created cookie.
            if (old_cookie.name != cookie.name)
                return IterationDecision::Continue;

            // 2. Their secure-only-flag is true.
            if (!old_cookie.secure)
                return IterationDecision::Continue;

            // 3. Their domain domain-matches (see Section 5.1.3) the domain of the newly-created cookie, or vice-versa.
            if (!HTTP::Cookie::domain_matches(old_cookie.domain, cookie.domain) && !HTTP::Cookie::domain_matches(cookie.domain, old_cookie.domain))
                return IterationDecision::Continue;

            // 4. The path of the newly-created cookie path-matches the path of the existing cookie.
            if (!HTTP::Cookie::path_matches(cookie.path, old_cookie.path))
                return IterationDecision::Continue;

            ignore_cookie = true;
            return IterationDecision::Break;
        });

        if (ignore_cookie)
            return;
    }

    // 17. If the cookie-attribute-list contains an attribute with an attribute-name of "SameSite", and an
    //     attribute-value of "Strict", "Lax", or "None", set the cookie's same-site-flag to the attribute-value of the
    //     last attribute in the cookie-attribute-list with an attribute-name of "SameSite". Otherwise, set the cookie's
    //     same-site-flag to "Default".
    cookie.same_site = parsed_cookie.same_site_attribute;

    // 18. If the cookie's same-site-flag is not "None":
    if (cookie.same_site != HTTP::Cookie::SameSite::None) {
        // FIXME: 1. If the cookie was received from a "non-HTTP" API, and the API was called from a navigable's active document
        //           whose "site for cookies" is not same-site with the top-level origin, then abort this algorithm and ignore
        //           the newly created cookie entirely.

        // FIXME: 2. If the cookie was received from a "same-site" request (as defined in Section 5.2), skip the remaining
        //           substeps and continue processing the cookie.

        // FIXME: 3. If the cookie was received from a request which is navigating a top-level traversable [HTML] (e.g. if the
        //           request's "reserved client" is either null or an environment whose "target browsing context"'s navigable
        //           is a top-level traversable), skip the remaining substeps and continue processing the cookie.

        // FIXME: 4. Abort this algorithm and ignore the newly created cookie entirely.
    }

    // 19. If the cookie's "same-site-flag" is "None", abort this algorithm and ignore the cookie entirely unless the
    //     cookie's secure-only-flag is true.
    if (cookie.same_site == HTTP::Cookie::SameSite::None && !cookie.secure)
        return;

    auto has_case_insensitive_prefix = [&](StringView value, StringView prefix) {
        if (value.length() < prefix.length())
            return false;

        value = value.substring_view(0, prefix.length());
        return value.equals_ignoring_ascii_case(prefix);
    };

    // 20. If the cookie-name begins with a case-insensitive match for the string "__Secure-", abort this algorithm and
    //     ignore the cookie entirely unless the cookie's secure-only-flag is true.
    if (has_case_insensitive_prefix(cookie.name, "__Secure-"sv) && !cookie.secure)
        return;

    // 21. If the cookie-name begins with a case-insensitive match for the string "__Host-", abort this algorithm and
    //     ignore the cookie entirely unless the cookie meets all the following criteria:
    if (has_case_insensitive_prefix(cookie.name, "__Host-"sv)) {
        // 1. The cookie's secure-only-flag is true.
        if (!cookie.secure)
            return;

        // 2. The cookie's host-only-flag is true.
        if (!cookie.host_only)
            return;

        // 3. The cookie-attribute-list contains an attribute with an attribute-name of "Path", and the cookie's path is /.
        if (parsed_cookie.path.has_value() && parsed_cookie.path != "/"sv)
            return;
    }

    // 22. If the cookie-name is empty and either of the following conditions are true, abort this algorithm and ignore
    //     the cookie entirely:
    if (cookie.name.is_empty()) {
        // * the cookie-value begins with a case-insensitive match for the string "__Secure-"
        if (has_case_insensitive_prefix(cookie.value, "__Secure-"sv))
            return;

        // * the cookie-value begins with a case-insensitive match for the string "__Host-"
        if (has_case_insensitive_prefix(cookie.value, "__Host-"sv))
            return;
    }

    CookieStorageKey key { cookie.name, cookie.domain, cookie.path };

    // 23. If the cookie store contains a cookie with the same name, domain, host-only-flag, and path as the
    //     newly-created cookie:
    if (auto const& old_cookie = m_transient_storage.get_cookie(key); old_cookie.has_value() && old_cookie->host_only == cookie.host_only) {
        // 1. Let old-cookie be the existing cookie with the same name, domain, host-only-flag, and path as the
        //    newly-created cookie. (Notice that this algorithm maintains the invariant that there is at most one such
        //    cookie.)

        // 2. If the newly-created cookie was received from a "non-HTTP" API and the old-cookie's http-only-flag is true,
        //    abort this algorithm and ignore the newly created cookie entirely.
        if (source == HTTP::Cookie::Source::NonHttp && old_cookie->http_only)
            return;

        // 3. Update the creation-time of the newly-created cookie to match the creation-time of the old-cookie.
        cookie.creation_time = old_cookie->creation_time;

        // 4. Remove the old-cookie from the cookie store.
        // NOTE: Rather than deleting then re-inserting this cookie, we update it in-place.
    }

    // 24. Insert the newly-created cookie into the cookie store.
    m_transient_storage.set_cookie(move(key), move(cookie));

    m_transient_storage.purge_expired_cookies();
}

// This is based on store_cookie() below, however the whole ParsedCookie->Cookie conversion is skipped.
void CookieJar::update_cookie(HTTP::Cookie::Cookie cookie)
{
    CookieStorageKey key { cookie.name, cookie.domain, cookie.path };

    // 23. If the cookie store contains a cookie with the same name, domain, host-only-flag, and path as the
    //     newly-created cookie:
    if (auto const& old_cookie = m_transient_storage.get_cookie(key); old_cookie.has_value() && old_cookie->host_only == cookie.host_only) {
        // 3. Update the creation-time of the newly-created cookie to match the creation-time of the old-cookie.
        cookie.creation_time = old_cookie->creation_time;

        // 4. Remove the old-cookie from the cookie store.
        // NOTE: Rather than deleting then re-inserting this cookie, we update it in-place.
    }

    // 24. Insert the newly-created cookie into the cookie store.
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
        builder.appendff("\t{}SameSite{} = {:s}\n", attribute_color, no_color, HTTP::Cookie::same_site_to_string(cookie.same_site));
    });

    dbgln("{} cookies stored\n{}", m_transient_storage.size(), builder.string_view());
}

Vector<HTTP::Cookie::Cookie> CookieJar::get_all_cookies()
{
    Vector<HTTP::Cookie::Cookie> cookies;
    cookies.ensure_capacity(m_transient_storage.size());

    m_transient_storage.for_each_cookie([&](auto const& cookie) {
        cookies.unchecked_append(cookie);
    });

    return cookies;
}

// https://w3c.github.io/webdriver/#dfn-associated-cookies
Vector<HTTP::Cookie::Cookie> CookieJar::get_all_cookies_webdriver(URL::URL const& url)
{
    return get_matching_cookies(url, HTTP::Cookie::Source::Http, MatchingCookiesSpecMode::WebDriver);
}

Vector<HTTP::Cookie::Cookie> CookieJar::get_all_cookies_cookiestore(URL::URL const& url)
{
    return get_matching_cookies(url, HTTP::Cookie::Source::NonHttp, MatchingCookiesSpecMode::RFC6265);
}

Optional<HTTP::Cookie::Cookie> CookieJar::get_named_cookie(URL::URL const& url, StringView name)
{
    auto cookie_list = get_matching_cookies(url, HTTP::Cookie::Source::Http, MatchingCookiesSpecMode::WebDriver);

    for (auto const& cookie : cookie_list) {
        if (cookie.name == name)
            return cookie;
    }

    return {};
}

void CookieJar::expire_cookies_with_time_offset(AK::Duration offset)
{
    m_transient_storage.purge_expired_cookies(offset);
}

void CookieJar::expire_cookies_accessed_since(UnixDateTime since)
{
    m_transient_storage.expire_and_purge_cookies_accessed_since(since);
}

Requests::CacheSizes CookieJar::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    return m_transient_storage.estimate_storage_size_accessed_since(since);
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-22#section-5.8.3
Vector<HTTP::Cookie::Cookie> CookieJar::get_matching_cookies(URL::URL const& url, HTTP::Cookie::Source source, MatchingCookiesSpecMode mode)
{
    auto now = UnixDateTime::now();

    // 1. Let retrieval-host-canonical be the canonicalized host of the retrieval's URI.
    auto retrieval_host_canonical = HTTP::Cookie::canonicalize_domain(url);

    // 2. If the host of the retrieval's URI fails to be canonicalized then abort this algorithm.
    if (!retrieval_host_canonical.has_value())
        return {};

    // 3. Let cookie-list be the set of cookies from the cookie store that meets all of the following requirements:
    Vector<HTTP::Cookie::Cookie> cookie_list;

    m_transient_storage.for_each_cookie([&](HTTP::Cookie::Cookie& cookie) {
        if (!HTTP::Cookie::cookie_matches_url(cookie, url, *retrieval_host_canonical, source))
            return;

        // NOTE: The WebDriver spec expects only step 1 above to be executed to match cookies.
        if (mode == MatchingCookiesSpecMode::WebDriver) {
            cookie_list.append(cookie);
            return;
        }

        // 5. Update the last-access-time of each cookie in the cookie-list to the current date and time.
        // NOTE: We do this first so that both our internal storage and cookie-list are updated.
        cookie.last_access_time = now;

        // 4. The user agent SHOULD sort the cookie-list in the following order:
        auto cookie_path_length = cookie.path.bytes().size();
        auto cookie_creation_time = cookie.creation_time;

        cookie_list.insert_before_matching(cookie, [cookie_path_length, cookie_creation_time](auto const& entry) {
            // * Cookies with longer paths are listed before cookies with shorter paths.
            if (cookie_path_length > entry.path.bytes().size()) {
                return true;
            }

            // * Among cookies that have equal-length path fields, cookies with earlier creation-times are listed
            //   before cookies with later creation-times.
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

void CookieJar::TransientStorage::set_cookie(CookieStorageKey key, HTTP::Cookie::Cookie cookie)
{
    auto now = UnixDateTime::now();

    // AD-HOC: Skip adding immediately-expiring cookies (i.e., only allow updating to immediately-expiring) to prevent
    //         firing deletion events for them.
    //         Spec issue: https://github.com/whatwg/cookiestore/issues/282
    if (cookie.expiry_time < now && !m_cookies.contains(key))
        return;

    // We skip notifying about updating expired cookies, as they will be notified as being expired immediately after instead
    if (cookie.expiry_time >= now) {
        auto cookie_value_changed = true;
        if (auto old_cookie = m_cookies.get(key); old_cookie.has_value())
            cookie_value_changed = old_cookie->value != cookie.value;

        send_cookie_changed_notifications({ { CookieEntry { {}, cookie } } }, cookie_value_changed);
    }

    m_cookies.set(key, cookie);
    m_dirty_cookies.set(move(key), move(cookie));
}

Optional<HTTP::Cookie::Cookie const&> CookieJar::TransientStorage::get_cookie(CookieStorageKey const& key)
{
    return m_cookies.get(key);
}

UnixDateTime CookieJar::TransientStorage::purge_expired_cookies(Optional<AK::Duration> offset)
{
    auto now = UnixDateTime::now();
    if (offset.has_value()) {
        now += *offset;

        for (auto& cookie : m_dirty_cookies)
            cookie.value.expiry_time -= *offset;
    }

    auto is_expired = [&](auto const&, auto const& cookie) { return cookie.expiry_time < now; };

    if (auto removed_entries = m_cookies.take_all_matching(is_expired); !removed_entries.is_empty())
        send_cookie_changed_notifications(removed_entries);

    return now;
}

void CookieJar::TransientStorage::expire_and_purge_cookies_accessed_since(UnixDateTime since)
{
    for (auto& [key, value] : m_cookies) {
        if (value.last_access_time >= since) {
            value.expiry_time = UnixDateTime::earliest();
            set_cookie(key, value);
        }
    }

    purge_expired_cookies();
}

Requests::CacheSizes CookieJar::TransientStorage::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    Requests::CacheSizes sizes;

    for (auto const& [key, value] : m_cookies) {
        auto size = key.name.byte_count() + key.domain.byte_count() + key.path.byte_count() + value.value.byte_count();
        sizes.total += size;

        if (value.last_access_time >= since)
            sizes.since_requested_time += size;
    }

    return sizes;
}

void CookieJar::TransientStorage::send_cookie_changed_notifications(ReadonlySpan<CookieEntry> cookies, bool inform_web_view_about_changed_domains)
{
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        auto retrieval_host_canonical = HTTP::Cookie::canonicalize_domain(view.url());
        if (!retrieval_host_canonical.has_value())
            return IterationDecision::Continue;

        HashTable<String> changed_domains;
        Vector<HTTP::Cookie::Cookie> matching_cookies;

        for (auto const& cookie : cookies) {
            if (inform_web_view_about_changed_domains)
                changed_domains.set(cookie.value.domain);

            if (HTTP::Cookie::cookie_matches_url(cookie.value, view.url(), *retrieval_host_canonical))
                matching_cookies.append(cookie.value);
        }

        view.notify_cookies_changed(changed_domains, matching_cookies);
        return IterationDecision::Continue;
    });
}

void CookieJar::PersistedStorage::insert_cookie(HTTP::Cookie::Cookie const& cookie)
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

static HTTP::Cookie::Cookie parse_cookie(Database::Database& database, Database::StatementID statement_id)
{
    int column = 0;
    auto convert_text = [&](auto& field) { field = database.result_column<String>(statement_id, column++); };
    auto convert_bool = [&](auto& field) { field = database.result_column<bool>(statement_id, column++); };
    auto convert_time = [&](auto& field) { field = database.result_column<UnixDateTime>(statement_id, column++); };

    auto convert_same_site = [&](auto& field) {
        auto same_site = database.result_column<UnderlyingType<HTTP::Cookie::SameSite>>(statement_id, column++);
        field = static_cast<HTTP::Cookie::SameSite>(same_site);
    };

    HTTP::Cookie::Cookie cookie;
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
    HashMap<CookieStorageKey, HTTP::Cookie::Cookie> cookies;

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
