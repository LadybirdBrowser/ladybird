/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ParsedCookie.h"
#include <AK/DateConstants.h>
#include <AK/Function.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/URL.h>
#include <LibWeb/Infra/Strings.h>
#include <ctype.h>

namespace Web::Cookie {

static void parse_attributes(URL::URL const&, ParsedCookie& parsed_cookie, StringView unparsed_attributes);
static void process_attribute(URL::URL const&, ParsedCookie& parsed_cookie, StringView attribute_name, StringView attribute_value);
static void on_expires_attribute(ParsedCookie& parsed_cookie, StringView attribute_value);
static void on_max_age_attribute(ParsedCookie& parsed_cookie, StringView attribute_value);
static void on_domain_attribute(ParsedCookie& parsed_cookie, StringView attribute_value);
static void on_path_attribute(URL::URL const&, ParsedCookie& parsed_cookie, StringView attribute_value);
static void on_secure_attribute(ParsedCookie& parsed_cookie);
static void on_http_only_attribute(ParsedCookie& parsed_cookie);
static void on_same_site_attribute(ParsedCookie& parsed_cookie, StringView attribute_value);
static Optional<UnixDateTime> parse_date_time(StringView date_string);

bool cookie_contains_invalid_control_character(StringView cookie_string)
{
    for (auto code_point : Utf8View { cookie_string }) {
        if (code_point <= 0x08)
            return true;
        if (code_point >= 0x0a && code_point <= 0x1f)
            return true;
        if (code_point == 0x7f)
            return true;
    }

    return false;
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6-6
Optional<ParsedCookie> parse_cookie(URL::URL const& url, StringView cookie_string)
{
    // 1. If the set-cookie-string contains a %x00-08 / %x0A-1F / %x7F character (CTL characters excluding HTAB):
    //    Abort these steps and ignore the set-cookie-string entirely.
    if (cookie_contains_invalid_control_character(cookie_string))
        return {};

    StringView name_value_pair;
    StringView unparsed_attributes;

    // 2. If the set-cookie-string contains a %x3B (";") character:
    if (auto position = cookie_string.find(';'); position.has_value()) {
        // 1. The name-value-pair string consists of the characters up to, but not including, the first %x3B (";"), and
        //    the unparsed-attributes consist of the remainder of the set-cookie-string (including the %x3B (";") in
        //    question).
        name_value_pair = cookie_string.substring_view(0, position.value());
        unparsed_attributes = cookie_string.substring_view(position.value());
    }
    // Otherwise:
    else {
        // 1. The name-value-pair string consists of all the characters contained in the set-cookie-string, and the
        //    unparsed-attributes is the empty string.
        name_value_pair = cookie_string;
    }

    StringView name;
    StringView value;

    // 3. If the name-value-pair string lacks a %x3D ("=") character, then the name string is empty, and the value
    //    string is the value of name-value-pair.
    if (auto position = name_value_pair.find('='); !position.has_value()) {
        value = name_value_pair;
    } else {
        // Otherwise, the name string consists of the characters up to, but not including, the first %x3D ("=") character
        // and the (possibly empty) value string consists of the characters after the first %x3D ("=") character.
        name = name_value_pair.substring_view(0, position.value());

        if (position.value() < name_value_pair.length() - 1)
            value = name_value_pair.substring_view(position.value() + 1);
    }

    // 4. Remove any leading or trailing WSP characters from the name string and the value string.
    name = name.trim_whitespace();
    value = value.trim_whitespace();

    // 5. If the sum of the lengths of the name string and the value string is more than 4096 octets, abort these steps
    //    and ignore the set-cookie-string entirely.
    if (name.length() + value.length() > 4096)
        return {};

    // 6. The cookie-name is the name string, and the cookie-value is the value string.
    ParsedCookie parsed_cookie { MUST(String::from_utf8(name)), MUST(String::from_utf8(value)) };

    parse_attributes(url, parsed_cookie, unparsed_attributes);
    return parsed_cookie;
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6-8
void parse_attributes(URL::URL const& url, ParsedCookie& parsed_cookie, StringView unparsed_attributes)
{
    // 1. If the unparsed-attributes string is empty, skip the rest of these steps.
    if (unparsed_attributes.is_empty())
        return;

    // 2. Discard the first character of the unparsed-attributes (which will be a %x3B (";") character).
    unparsed_attributes = unparsed_attributes.substring_view(1);

    StringView cookie_av;

    // 3. If the remaining unparsed-attributes contains a %x3B (";") character:
    if (auto position = unparsed_attributes.find(';'); position.has_value()) {
        // 1. Consume the characters of the unparsed-attributes up to, but not including, the first %x3B (";") character.
        cookie_av = unparsed_attributes.substring_view(0, position.value());
        unparsed_attributes = unparsed_attributes.substring_view(position.value());
    }
    // Otherwise:
    else {
        // 1. Consume the remainder of the unparsed-attributes.
        cookie_av = unparsed_attributes;
        unparsed_attributes = {};
    }
    // Let the cookie-av string be the characters consumed in this step.

    StringView attribute_name;
    StringView attribute_value;

    // 4. If the cookie-av string contains a %x3D ("=") character:
    if (auto position = cookie_av.find('='); position.has_value()) {
        // 1. The (possibly empty) attribute-name string consists of the characters up to, but not including, the first
        //    %x3D ("=") character, and the (possibly empty) attribute-value string consists of the characters after the
        //    first %x3D ("=") character.
        attribute_name = cookie_av.substring_view(0, position.value());

        if (position.value() < cookie_av.length() - 1)
            attribute_value = cookie_av.substring_view(position.value() + 1);
    }
    // Otherwise:
    else {
        // 1. The attribute-name string consists of the entire cookie-av string, and the attribute-value string is empty.
        attribute_name = cookie_av;
    }

    // 5. Remove any leading or trailing WSP characters from the attribute-name string and the attribute-value string.
    attribute_name = attribute_name.trim_whitespace();
    attribute_value = attribute_value.trim_whitespace();

    // 6. If the attribute-value is longer than 1024 octets, ignore the cookie-av string and return to Step 1 of this
    //    algorithm.
    if (attribute_value.length() > 1024) {
        parse_attributes(url, parsed_cookie, unparsed_attributes);
        return;
    }

    // 7. Process the attribute-name and attribute-value according to the requirements in the following subsections.
    //    (Notice that attributes with unrecognized attribute-names are ignored.)
    process_attribute(url, parsed_cookie, attribute_name, attribute_value);

    // 8. Return to Step 1 of this algorithm.
    parse_attributes(url, parsed_cookie, unparsed_attributes);
}

void process_attribute(URL::URL const& url, ParsedCookie& parsed_cookie, StringView attribute_name, StringView attribute_value)
{
    if (attribute_name.equals_ignoring_ascii_case("Expires"sv)) {
        on_expires_attribute(parsed_cookie, attribute_value);
    } else if (attribute_name.equals_ignoring_ascii_case("Max-Age"sv)) {
        on_max_age_attribute(parsed_cookie, attribute_value);
    } else if (attribute_name.equals_ignoring_ascii_case("Domain"sv)) {
        on_domain_attribute(parsed_cookie, attribute_value);
    } else if (attribute_name.equals_ignoring_ascii_case("Path"sv)) {
        on_path_attribute(url, parsed_cookie, attribute_value);
    } else if (attribute_name.equals_ignoring_ascii_case("Secure"sv)) {
        on_secure_attribute(parsed_cookie);
    } else if (attribute_name.equals_ignoring_ascii_case("HttpOnly"sv)) {
        on_http_only_attribute(parsed_cookie);
    } else if (attribute_name.equals_ignoring_ascii_case("SameSite"sv)) {
        on_same_site_attribute(parsed_cookie, attribute_value);
    }
}

static constexpr AK::Duration maximum_cookie_age()
{
    return AK::Duration::from_seconds(400LL * 24 * 60 * 60);
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.1
void on_expires_attribute(ParsedCookie& parsed_cookie, StringView attribute_value)
{
    // 1. Let the expiry-time be the result of parsing the attribute-value as cookie-date (see Section 5.1.1).
    auto expiry_time = parse_date_time(attribute_value);

    // 2. If the attribute-value failed to parse as a cookie date, ignore the cookie-av.
    if (!expiry_time.has_value())
        return;

    // 3. Let cookie-age-limit be the maximum age of the cookie (which SHOULD be 400 days in the future or sooner, see
    //    Section 5.5).
    auto cookie_age_limit = UnixDateTime::now() + maximum_cookie_age();

    // 4. If the expiry-time is more than cookie-age-limit, the user agent MUST set the expiry time to cookie-age-limit
    //    in seconds.
    if (expiry_time->seconds_since_epoch() > cookie_age_limit.seconds_since_epoch())
        expiry_time = cookie_age_limit;

    // 5. If the expiry-time is earlier than the earliest date the user agent can represent, the user agent MAY replace
    //    the expiry-time with the earliest representable date.
    if (auto earliest = UnixDateTime::earliest(); *expiry_time < earliest)
        expiry_time = earliest;

    // 6. Append an attribute to the cookie-attribute-list with an attribute-name of Expires and an attribute-value of
    //    expiry-time.
    parsed_cookie.expiry_time_from_expires_attribute = expiry_time.release_value();
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.2
void on_max_age_attribute(ParsedCookie& parsed_cookie, StringView attribute_value)
{
    // 1. If the attribute-value is empty, ignore the cookie-av.
    if (attribute_value.is_empty())
        return;

    // 2. If the first character of the attribute-value is neither a DIGIT, nor a "-" character followed by a DIGIT,
    //    ignore the cookie-av.
    // 3. If the remainder of attribute-value contains a non-DIGIT character, ignore the cookie-av.
    auto digits = attribute_value[0] == '-' ? attribute_value.substring_view(1) : attribute_value;

    if (digits.is_empty() || !all_of(digits, is_ascii_digit))
        return;

    // 4. Let delta-seconds be the attribute-value converted to a base 10 integer.
    auto delta_seconds = attribute_value.to_number<i64>();
    if (!delta_seconds.has_value()) {
        // We know the attribute value only contains digits, so if we failed to parse, it is because the result did not
        // fit in an i64. Set the value to the i64 limits in that case. The positive limit will be further capped below,
        // and the negative limit will be immediately expired in the cookie jar.
        delta_seconds = attribute_value[0] == '-' ? NumericLimits<i64>::min() : NumericLimits<i64>::max();
    }

    // 5. Let cookie-age-limit be the maximum age of the cookie (which SHOULD be 400 days or less, see Section 5.5).
    auto cookie_age_limit = maximum_cookie_age();

    // 6. Set delta-seconds to the smaller of its present value and cookie-age-limit.
    if (*delta_seconds > cookie_age_limit.to_seconds())
        delta_seconds = cookie_age_limit.to_seconds();

    // 7. If delta-seconds is less than or equal to zero (0), let expiry-time be the earliest representable date and
    //    time. Otherwise, let the expiry-time be the current date and time plus delta-seconds seconds.
    auto expiry_time = *delta_seconds <= 0
        ? UnixDateTime::earliest()
        : UnixDateTime::now() + AK::Duration::from_seconds(*delta_seconds);

    // 8. Append an attribute to the cookie-attribute-list with an attribute-name of Max-Age and an attribute-value of
    //    expiry-time.
    parsed_cookie.expiry_time_from_max_age_attribute = expiry_time;
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.3
void on_domain_attribute(ParsedCookie& parsed_cookie, StringView attribute_value)
{
    // 1. Let cookie-domain be the attribute-value.
    auto cookie_domain = attribute_value;

    // 2. If cookie-domain starts with %x2E ("."), let cookie-domain be cookie-domain without its leading %x2E (".").
    if (cookie_domain.starts_with('.'))
        cookie_domain = cookie_domain.substring_view(1);

    // 3. Convert the cookie-domain to lower case.
    auto lowercase_cookie_domain = MUST(Infra::to_ascii_lowercase(cookie_domain));

    // 4. Append an attribute to the cookie-attribute-list with an attribute-name of Domain and an attribute-value of
    //    cookie-domain.
    parsed_cookie.domain = move(lowercase_cookie_domain);
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.4
void on_path_attribute(URL::URL const& url, ParsedCookie& parsed_cookie, StringView attribute_value)
{
    String cookie_path;

    // 1. If the attribute-value is empty or if the first character of the attribute-value is not %x2F ("/"):
    if (attribute_value.is_empty() || attribute_value[0] != '/') {
        // 1. Let cookie-path be the default-path.
        cookie_path = default_path(url);
    }
    // Otherwise:
    else {
        // 1. Let cookie-path be the attribute-value.
        cookie_path = MUST(String::from_utf8(attribute_value));
    }

    // 2. Append an attribute to the cookie-attribute-list with an attribute-name of Path and an attribute-value of
    //    cookie-path.
    parsed_cookie.path = move(cookie_path);
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.5
void on_secure_attribute(ParsedCookie& parsed_cookie)
{
    parsed_cookie.secure_attribute_present = true;
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.6
void on_http_only_attribute(ParsedCookie& parsed_cookie)
{
    parsed_cookie.http_only_attribute_present = true;
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.7
void on_same_site_attribute(ParsedCookie& parsed_cookie, StringView attribute_value)
{
    // 1. Let enforcement be "Default".
    // 2. If cookie-av's attribute-value is a case-insensitive match for "None", set enforcement to "None".
    // 3. If cookie-av's attribute-value is a case-insensitive match for "Strict", set enforcement to "Strict".
    // 4. If cookie-av's attribute-value is a case-insensitive match for "Lax", set enforcement to "Lax".
    auto enforcement = same_site_from_string(attribute_value);

    // 5. Append an attribute to the cookie-attribute-list with an attribute-name of "SameSite" and an attribute-value
    //    of enforcement.
    parsed_cookie.same_site_attribute = enforcement;
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.1.1
Optional<UnixDateTime> parse_date_time(StringView date_string)
{
    // https://tools.ietf.org/html/rfc6265#section-5.1.1
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    unsigned day_of_month = 0;
    unsigned month = 0;
    unsigned year = 0;

    auto to_uint = [](StringView token, unsigned& result) {
        if (!all_of(token, isdigit))
            return false;

        if (auto converted = token.to_number<unsigned>(); converted.has_value()) {
            result = *converted;
            return true;
        }

        return false;
    };

    auto parse_time = [&](StringView token) {
        Vector<StringView> parts = token.split_view(':');
        if (parts.size() != 3)
            return false;

        for (auto const& part : parts) {
            if (part.is_empty() || part.length() > 2)
                return false;
        }

        return to_uint(parts[0], hour) && to_uint(parts[1], minute) && to_uint(parts[2], second);
    };

    auto parse_day_of_month = [&](StringView token) {
        if (token.is_empty() || token.length() > 2)
            return false;
        return to_uint(token, day_of_month);
    };

    auto parse_month = [&](StringView token) {
        for (unsigned i = 0; i < 12; ++i) {
            if (token.equals_ignoring_ascii_case(short_month_names[i])) {
                month = i + 1;
                return true;
            }
        }

        return false;
    };

    auto parse_year = [&](StringView token) {
        if (token.length() != 2 && token.length() != 4)
            return false;
        return to_uint(token, year);
    };

    Function<bool(char)> is_delimiter = [](char ch) {
        return ch == 0x09 || (ch >= 0x20 && ch <= 0x2f) || (ch >= 0x3b && ch <= 0x40) || (ch >= 0x5b && ch <= 0x60) || (ch >= 0x7b && ch <= 0x7e);
    };

    // 1. Using the grammar below, divide the cookie-date into date-tokens.
    Vector<StringView> date_tokens = date_string.split_view_if(is_delimiter);

    // 2. Process each date-token sequentially in the order the date-tokens appear in the cookie-date:
    bool found_time = false;
    bool found_day_of_month = false;
    bool found_month = false;
    bool found_year = false;

    for (auto const& date_token : date_tokens) {
        // 1. If the found-time flag is not set and the token matches the time production, set the found-time flag and
        //    set the hour-value, minute-value, and second-value to the numbers denoted by the digits in the date-token,
        //    respectively. Skip the remaining sub-steps and continue to the next date-token.
        if (!found_time && parse_time(date_token)) {
            found_time = true;
        }

        // 2. If the found-day-of-month flag is not set and the date-token matches the day-of-month production, set the
        //    found-day-of-month flag and set the day-of-month-value to the number denoted by the date-token. Skip the
        //    remaining sub-steps and continue to the next date-token.
        else if (!found_day_of_month && parse_day_of_month(date_token)) {
            found_day_of_month = true;
        }

        // 3. If the found-month flag is not set and the date-token matches the month production, set the found-month
        //    flag and set the month-value to the month denoted by the date-token. Skip the remaining sub-steps and
        //    continue to the next date-token.
        else if (!found_month && parse_month(date_token)) {
            found_month = true;
        }

        // 4. If the found-year flag is not set and the date-token matches the year production, set the found-year flag
        //    and set the year-value to the number denoted by the date-token. Skip the remaining sub-steps and continue
        //    to the next date-token.
        else if (!found_year && parse_year(date_token)) {
            found_year = true;
        }
    }

    // 3. If the year-value is greater than or equal to 70 and less than or equal to 99, increment the year-value by 1900.
    if (year >= 70 && year <= 99)
        year += 1900;

    // 4. If the year-value is greater than or equal to 0 and less than or equal to 69, increment the year-value by 2000.
    if (year <= 69)
        year += 2000;

    // 5. Abort these steps and fail to parse the cookie-date if:
    // * at least one of the found-day-of-month, found-month, found-year, or found-time flags is not set,
    if (!found_day_of_month || !found_month || !found_year || !found_time)
        return {};
    // * the day-of-month-value is less than 1 or greater than 31,
    if (day_of_month < 1 || day_of_month > 31)
        return {};
    // * the year-value is less than 1601,
    if (year < 1601)
        return {};
    // * the hour-value is greater than 23,
    if (hour > 23)
        return {};
    // * the minute-value is greater than 59, or
    if (minute > 59)
        return {};
    // * the second-value is greater than 59.
    if (second > 59)
        return {};

    // 6. Let the parsed-cookie-date be the date whose day-of-month, month, year, hour, minute, and second (in UTC) are the
    //    day-of-month-value, the month-value, the year-value, the hour-value, the minute-value, and the second-value, respectively.
    //    If no such date exists, abort these steps and fail to parse the cookie-date.
    if (day_of_month > static_cast<unsigned int>(days_in_month(year, month)))
        return {};

    // FIXME: This currently uses UNIX time, which is not equivalent to UTC due to leap seconds.
    auto parsed_cookie_date = UnixDateTime::from_unix_time_parts(year, month, day_of_month, hour, minute, second, 0);

    // 7. Return the parsed-cookie-date as the result of this algorithm.
    return parsed_cookie_date;
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.1.4
String default_path(URL::URL const& url)
{
    // 1. Let uri-path be the path portion of the request-uri if such a portion exists (and empty otherwise).
    auto uri_path = URL::percent_decode(url.serialize_path());

    // 2. If the uri-path is empty or if the first character of the uri-path is not a %x2F ("/") character, output
    //    %x2F ("/") and skip the remaining steps.
    if (uri_path.is_empty() || (uri_path[0] != '/'))
        return "/"_string;

    StringView uri_path_view = uri_path;
    size_t last_separator = uri_path_view.find_last('/').value();

    // 3. If the uri-path contains no more than one %x2F ("/") character, output %x2F ("/") and skip the remaining step.
    if (last_separator == 0)
        return "/"_string;

    // 4. Output the characters of the uri-path from the first character up to, but not including, the right-most
    //    %x2F ("/").
    // FIXME: The path might not be valid UTF-8.
    return MUST(String::from_utf8(uri_path.substring_view(0, last_separator)));
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Cookie::ParsedCookie const& cookie)
{
    TRY(encoder.encode(cookie.name));
    TRY(encoder.encode(cookie.value));
    TRY(encoder.encode(cookie.expiry_time_from_expires_attribute));
    TRY(encoder.encode(cookie.expiry_time_from_max_age_attribute));
    TRY(encoder.encode(cookie.domain));
    TRY(encoder.encode(cookie.path));
    TRY(encoder.encode(cookie.secure_attribute_present));
    TRY(encoder.encode(cookie.http_only_attribute_present));
    TRY(encoder.encode(cookie.same_site_attribute));

    return {};
}

template<>
ErrorOr<Web::Cookie::ParsedCookie> IPC::decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<String>());
    auto value = TRY(decoder.decode<String>());
    auto expiry_time_from_expires_attribute = TRY(decoder.decode<Optional<UnixDateTime>>());
    auto expiry_time_from_max_age_attribute = TRY(decoder.decode<Optional<UnixDateTime>>());
    auto domain = TRY(decoder.decode<Optional<String>>());
    auto path = TRY(decoder.decode<Optional<String>>());
    auto secure_attribute_present = TRY(decoder.decode<bool>());
    auto http_only_attribute_present = TRY(decoder.decode<bool>());
    auto same_site_attribute = TRY(decoder.decode<Web::Cookie::SameSite>());

    return Web::Cookie::ParsedCookie { move(name), move(value), same_site_attribute, move(expiry_time_from_expires_attribute), move(expiry_time_from_max_age_attribute), move(domain), move(path), secure_attribute_present, http_only_attribute_present };
}
