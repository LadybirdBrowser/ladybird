/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibHTTP/HTTP.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace HTTP::HSTS {

// https://www.rfc-editor.org/rfc/rfc7230#section-3.2.6
static Optional<String> consume_quoted_string(GenericLexer& lexer)
{
    if (lexer.is_eof() || lexer.peek() != '"')
        return {};
    lexer.ignore(1);

    StringBuilder value;
    while (!lexer.is_eof()) {
        char ch = lexer.consume();
        if (ch == '"') {
            // RFC 7230 quoted-string permits obs-text = %x80-FF, so the collected bytes are not
            // guaranteed to be valid UTF-8. Treat a malformed encoding as a parse error rather
            // than crashing.
            auto result = value.to_string();
            if (result.is_error())
                return {};
            return result.release_value();
        }
        if (ch == '\\') {
            if (lexer.is_eof())
                return {};
            value.append(lexer.consume());
            continue;
        }
        value.append(ch);
    }
    return {};
}

// https://www.rfc-editor.org/rfc/rfc6797#section-6.1
// Strict-Transport-Security = "Strict-Transport-Security" ":"
//                             [ directive ]  *( ";" [ directive ] )
// directive                 = directive-name [ "=" directive-value ]
// directive-name            = token
// directive-value           = token | quoted-string
Optional<ParsedHSTSPolicy> parse_header(StringView header_value)
{
    GenericLexer lexer(header_value);

    Optional<u64> max_age;
    bool include_sub_domains = false;

    // 1. The order of appearance of directives is not significant.
    while (!lexer.is_eof()) {
        lexer.ignore_while(is_http_tab_or_space);

        if (lexer.is_eof())
            break;

        // 3. Directive names are case-insensitive.
        auto directive_name = lexer.consume_until([](char ch) {
            return ch == '=' || ch == ';';
        });
        directive_name = directive_name.trim(HTTP_TAB_OR_SPACE, TrimMode::Both);

        // directive-name = token. Empty is only valid when no value follows (i.e. successive ';'
        // separators or trailing ';'); a non-empty name must be a valid token.
        if (directive_name.is_empty()) {
            if (!lexer.is_eof() && lexer.peek() == '=')
                return {};
        } else if (!is_token(directive_name)) {
            return {};
        }

        Optional<String> directive_value;
        if (!lexer.is_eof() && lexer.peek() == '=') {
            lexer.ignore(1);
            lexer.ignore_while(is_http_tab_or_space);

            if (!lexer.is_eof() && lexer.peek() == '"') {
                directive_value = consume_quoted_string(lexer);
                if (!directive_value.has_value())
                    return {};

                // NB: A quoted-string directive-value must end at a directive boundary; trailing
                //     token characters are malformed.
                lexer.ignore_while(is_http_tab_or_space);
                if (!lexer.is_eof() && lexer.peek() != ';')
                    return {};
            } else {
                auto token_value = lexer.consume_until(';').trim(HTTP_TAB_OR_SPACE, TrimMode::Both);
                if (!is_token(token_value))
                    return {};
                // NB: is_token guarantees ASCII-only bytes, which are trivially valid UTF-8.
                directive_value = MUST(String::from_utf8(token_value));
            }
        }

        if (directive_name.equals_ignoring_ascii_case("max-age"sv)) {
            // https://www.rfc-editor.org/rfc/rfc6797#section-6.1.1
            // The REQUIRED "max-age" directive specifies the number of seconds, after the reception of the STS header
            // field, during which the UA regards the host (from whom the message was received) as a Known HSTS Host.
            // max-age-value = delta-seconds
            // delta-seconds = 1*DIGIT
            if (!directive_value.has_value())
                return {};

            auto parsed = directive_value->bytes_as_string_view().to_number<u64>();
            if (!parsed.has_value())
                return {};

            // 2. All directives MUST appear only once in an STS header field.
            if (max_age.has_value())
                return {};

            max_age = parsed.value();
        } else if (directive_name.equals_ignoring_ascii_case("includeSubDomains"sv)) {
            // https://www.rfc-editor.org/rfc/rfc6797#section-6.1.2
            // The OPTIONAL "includeSubDomains" directive is a valueless directive which, if present (i.e., it is
            // "asserted"), signals the UA that the HSTS Policy applies to this HSTS Host as well as any subdomains
            // of the host's domain name.

            // NB: An asserted value on a valueless directive (e.g. "includeSubDomains=anything") is malformed.
            if (directive_value.has_value())
                return {};

            // 2. All directives MUST appear only once in an STS header field.
            if (include_sub_domains)
                return {};

            include_sub_domains = true;
        }
        // 5. If an STS header field contains directive(s) not recognized by the UA, the UA MUST ignore the
        //    unrecognized directives, and if the STS header field otherwise satisfies the above requirements
        //    (1 through 4), the UA MUST process the recognized directives.

        if (!lexer.is_eof() && lexer.peek() == ';')
            lexer.ignore(1);
    }

    // 4. UAs MUST ignore any STS header field containing directives, or other header field value data,
    //    that does not conform to the syntax defined in this specification.
    // The max-age directive is REQUIRED.
    if (!max_age.has_value())
        return {};

    // NB: Clamp delta-seconds to i64::max so AK::Duration::from_seconds cannot overflow.
    auto clamped_seconds = AK::min<u64>(*max_age, NumericLimits<i64>::max());
    return ParsedHSTSPolicy {
        .max_age = AK::Duration::from_seconds(static_cast<i64>(clamped_seconds)),
        .include_sub_domains = include_sub_domains,
    };
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, HTTP::HSTS::ParsedHSTSPolicy const& policy)
{
    TRY(encoder.encode(policy.max_age));
    TRY(encoder.encode(policy.include_sub_domains));
    return {};
}

template<>
ErrorOr<HTTP::HSTS::ParsedHSTSPolicy> IPC::decode(Decoder& decoder)
{
    auto max_age = TRY(decoder.decode<AK::Duration>());
    auto include_sub_domains = TRY(decoder.decode<bool>());
    return HTTP::HSTS::ParsedHSTSPolicy { max_age, include_sub_domains };
}
