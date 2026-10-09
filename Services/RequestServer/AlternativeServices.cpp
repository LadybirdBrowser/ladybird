/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/StringBuilder.h>
#include <LibURL/URL.h>
#include <RequestServer/AlternativeServices.h>

namespace RequestServer {

// https://www.rfc-editor.org/rfc/rfc7838#section-3.1
// When an alternative service is advertised using Alt-Svc, it is considered fresh for 24 hours from generation of the
// message.
static constexpr i64 default_max_age_seconds = 86400;
static constexpr i64 maximum_max_age_seconds = static_cast<i64>(86400) * 365;

static bool is_tchar(char c)
{
    return is_ascii_alphanumeric(c) || "!#$%&'*+-.^_`|~"sv.contains(c);
}

static void skip_ows(GenericLexer& lexer)
{
    lexer.ignore_while([](char c) { return c == ' ' || c == '\t'; });
}

static Optional<StringView> consume_token(GenericLexer& lexer)
{
    auto token = lexer.consume_while(is_tchar);
    if (token.is_empty())
        return {};
    return token;
}

static Optional<ByteString> consume_quoted_string(GenericLexer& lexer)
{
    if (!lexer.consume_specific('"'))
        return {};

    StringBuilder builder;
    while (!lexer.is_eof()) {
        auto c = lexer.consume();
        if (c == '"')
            return builder.to_byte_string();
        if (c == '\\') {
            if (lexer.is_eof())
                return {};
            c = lexer.consume();
        }
        builder.append(c);
    }
    return {};
}

static Optional<ByteString> percent_decode(StringView string)
{
    StringBuilder builder;
    for (size_t i = 0; i < string.length(); ++i) {
        if (string[i] != '%') {
            builder.append(string[i]);
            continue;
        }
        if (i + 2 >= string.length() || !is_ascii_hex_digit(string[i + 1]) || !is_ascii_hex_digit(string[i + 2]))
            return {};
        builder.append(static_cast<char>((parse_ascii_hex_digit(string[i + 1]) * 16) + parse_ascii_hex_digit(string[i + 2])));
        i += 2;
    }
    return builder.to_byte_string();
}

// alt-authority = quoted-string ; containing [ uri-host ] ":" port
static bool parse_alt_authority(StringView authority, AlternativeService& alternative)
{
    auto port_start = authority.find_last(':');
    if (!port_start.has_value())
        return false;

    auto host = authority.substring_view(0, *port_start);
    if (host.starts_with('[')) {
        if (!host.ends_with(']'))
            return false;
        host = host.substring_view(1, host.length() - 2);
    } else if (host.contains(':')) {
        return false;
    }
    if (!all_of(host, [](char c) { return is_ascii_alphanumeric(c) || c == '-' || c == '.' || c == ':'; }))
        return false;

    auto port = authority.substring_view(*port_start + 1).to_number<u16>();
    if (!port.has_value() || *port == 0)
        return false;

    alternative.host = host.to_byte_string().to_lowercase();
    alternative.port = *port;
    return true;
}

// https://www.rfc-editor.org/rfc/rfc7838#section-3
// Alt-Svc       = clear / 1#alt-value
// clear         = %s"clear"; "clear", case-sensitive
// alt-value     = alternative *( OWS ";" OWS parameter )
// alternative   = protocol-id "=" alt-authority
// protocol-id   = token ; percent-encoded ALPN protocol name
// alt-authority = quoted-string ; containing [ uri-host ] ":" port
// parameter     = token "=" ( token / quoted-string )
Optional<Vector<AlternativeService>> parse_alt_svc(StringView value, UnixDateTime now, AK::Duration age)
{
    value = value.trim(" \t"sv);
    if (value == "clear"sv)
        return Vector<AlternativeService> {};

    Vector<AlternativeService> alternatives;
    size_t alt_value_count = 0;
    GenericLexer lexer { value };

    while (true) {
        lexer.ignore_while([](char c) { return c == ' ' || c == '\t' || c == ','; });
        if (lexer.is_eof())
            break;

        auto protocol_id = consume_token(lexer);
        if (!protocol_id.has_value() || !lexer.consume_specific('='))
            return {};
        auto authority = consume_quoted_string(lexer);
        if (!authority.has_value())
            return {};

        AlternativeService alternative;
        auto is_usable = parse_alt_authority(*authority, alternative);

        auto protocol = percent_decode(*protocol_id);
        if (!protocol.has_value())
            return {};
        if (*protocol != "h3"sv)
            is_usable = false;

        i64 max_age = default_max_age_seconds;
        while (true) {
            skip_ows(lexer);
            if (!lexer.consume_specific(';'))
                break;
            skip_ows(lexer);

            auto name = consume_token(lexer);
            if (!name.has_value() || !lexer.consume_specific('='))
                return {};

            Optional<ByteString> parameter_value;
            if (lexer.next_is('"'))
                parameter_value = consume_quoted_string(lexer);
            else if (auto token = consume_token(lexer); token.has_value())
                parameter_value = token->to_byte_string();
            if (!parameter_value.has_value())
                return {};

            // https://www.rfc-editor.org/rfc/rfc7838#section-3.1
            // The delta-seconds value indicates the number of seconds since the response was generated for which the
            // alternative service is considered fresh.
            if (name->equals_ignoring_ascii_case("ma"sv)) {
                if (parameter_value->is_empty() || !all_of(*parameter_value, is_ascii_digit))
                    return {};
                max_age = parameter_value->to_number<i64>().value_or(maximum_max_age_seconds);
            }
        }

        skip_ows(lexer);
        if (!lexer.is_eof() && !lexer.next_is(','))
            return {};

        ++alt_value_count;
        if (!is_usable)
            continue;

        alternative.received_at = now;
        alternative.expires_at = now + AK::Duration::from_seconds(min(max_age, maximum_max_age_seconds)) - age;
        alternatives.append(move(alternative));
    }

    if (alt_value_count == 0)
        return {};
    return alternatives;
}

AlternativeServiceCache& AlternativeServiceCache::the()
{
    static AlternativeServiceCache s_cache;
    return s_cache;
}

ByteString AlternativeServiceCache::key_for(IsPrivate is_private, URL::URL const& origin)
{
    return ByteString::formatted("{}|{}:{}", is_private == IsPrivate::Yes ? 'p' : 'n', origin.serialized_host(), origin.port_or_default());
}

void AlternativeServiceCache::update(IsPrivate is_private, URL::URL const& origin, StringView alt_svc, UnixDateTime now, AK::Duration age)
{
    auto alternatives = parse_alt_svc(alt_svc, now, age);
    if (!alternatives.has_value())
        return;

    auto key = key_for(is_private, origin);

    // https://www.rfc-editor.org/rfc/rfc7838#section-3.1
    // When an Alt-Svc response header field is received from an origin, its value invalidates and replaces all cached
    // alternative services for that origin.
    if (alternatives->is_empty()) {
        m_alternatives.remove(key);
        return;
    }

    if (alternatives->size() > maximum_alternatives_per_origin)
        alternatives->shrink(maximum_alternatives_per_origin);

    if (!m_alternatives.contains(key) && m_alternatives.size() >= maximum_origins) {
        auto oldest = m_alternatives.begin();
        for (auto it = m_alternatives.begin(); it != m_alternatives.end(); ++it) {
            if (it->value.first().received_at < oldest->value.first().received_at)
                oldest = it;
        }
        m_alternatives.remove(oldest);
    }

    m_alternatives.set(move(key), alternatives.release_value());
}

ByteString AlternativeServiceCache::broken_key_for(ByteString const& origin_key, AlternativeService const& alternative)
{
    return ByteString::formatted("{}|{}:{}", origin_key, alternative.host, alternative.port);
}

Optional<AlternativeService> AlternativeServiceCache::find(IsPrivate is_private, URL::URL const& origin, UnixDateTime now)
{
    auto key = key_for(is_private, origin);
    auto alternatives = m_alternatives.find(key);
    if (alternatives == m_alternatives.end())
        return {};

    alternatives->value.remove_all_matching([&](auto const& alternative) { return alternative.expires_at <= now; });
    if (alternatives->value.is_empty()) {
        m_alternatives.remove(alternatives);
        return {};
    }

    m_broken_until.remove_all_matching([&](auto const&, auto until) { return until <= now; });

    // The field value lists alternatives in the server's order of preference.
    for (auto const& alternative : alternatives->value) {
        if (!m_broken_until.contains(broken_key_for(key, alternative)))
            return alternative;
    }
    return {};
}

void AlternativeServiceCache::mark_broken(IsPrivate is_private, URL::URL const& origin, AlternativeService const& alternative, UnixDateTime now)
{
    m_broken_until.set(broken_key_for(key_for(is_private, origin), alternative), now + broken_alternative_timeout);
}

void AlternativeServiceCache::remove_entries_received_since(UnixDateTime since)
{
    m_broken_until.clear();

    m_alternatives.remove_all_matching([&](auto const&, auto& alternatives) {
        alternatives.remove_all_matching([&](auto const& alternative) { return alternative.received_at >= since; });
        return alternatives.is_empty();
    });
}

void AlternativeServiceCache::clear(IsPrivate is_private)
{
    auto prefix = is_private == IsPrivate::Yes ? "p|"sv : "n|"sv;
    m_alternatives.remove_all_matching([&](auto const& key, auto const&) { return key.starts_with(prefix); });
    m_broken_until.remove_all_matching([&](auto const& key, auto const&) { return key.starts_with(prefix); });
}

}
