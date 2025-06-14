/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SourceExpression.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#source-expression
class SourceExpressionParser {
public:
    explicit SourceExpressionParser(StringView input)
        : m_input(input)
        , m_state({
              .lexer = GenericLexer { input },
              .parse_result = {},
          })
    {
    }

    [[nodiscard]] GenericLexer const& lexer() const { return m_state.lexer; }
    [[nodiscard]] SourceExpressionParseResult const& parse_result() const { return m_state.parse_result; }

    // https://w3c.github.io/webappsec-csp/#grammardef-scheme-source
    [[nodiscard]] bool parse_scheme_source()
    {
        // ; Schemes: "https:" / "custom-scheme:" / "another.custom-scheme:"
        // scheme-source = scheme-part ":"
        if (!parse_scheme_part())
            return false;

        return m_state.lexer.consume_specific(':');
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-scheme-part
    [[nodiscard]] bool parse_scheme_part()
    {
        // scheme-part = scheme
        // ; scheme is defined in section 3.1 of RFC 3986.
        StateTransaction transaction { *this };

        if (!parse_scheme())
            return false;

        m_state.parse_result.scheme_part = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-3.1
    [[nodiscard]] bool parse_scheme()
    {
        // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
        if (!m_state.lexer.consume_specific_with_predicate(is_ascii_alpha))
            return false;

        (void)m_state.lexer.consume_while([](char ch) {
            return is_ascii_alpha(ch) || is_ascii_digit(ch) || ch == '+' || ch == '-' || ch == '.';
        });
        return true;
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-host-source
    [[nodiscard]] bool parse_host_source()
    {
        // ; Hosts: "example.com" / "*.example.com" / "https://*.example.com:12/path/to/file.js"
        // host-source = [ scheme-part "://" ] host-part [ ":" port-part ] [ path-part ]
        auto parse_scheme = [&] {
            StateTransaction transaction { *this };

            if (!parse_scheme_part())
                return;

            if (!m_state.lexer.consume_specific("://"sv)) {
                m_state.parse_result.scheme_part = OptionalNone {};
                return;
            }

            transaction.commit();
        };

        parse_scheme();

        if (!parse_host_part())
            return false;

        if (m_state.lexer.consume_specific(':')) {
            if (!parse_port_part())
                return false;
        }

        (void)parse_path_part();

        return true;
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-host-part
    [[nodiscard]] bool parse_host_part()
    {
        // host-part = "*" / [ "*." ] 1*host-char *( "." 1*host-char ) [ "." ]
        StateTransaction transaction { *this };

        if (m_state.lexer.consume_specific('*') && !m_state.lexer.consume_specific('.')) {
            m_state.parse_result.host_part = transaction.parsed_string_view();
            transaction.commit();
            return true;
        }

        if (!parse_host_char())
            return false;

        while (parse_host_char())
            ;

        while (m_state.lexer.consume_specific('.')) {
            if (parse_host_char()) {
                while (parse_host_char())
                    ;
            } else {
                break;
            }
        }

        m_state.parse_result.host_part = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-host-char
    [[nodiscard]] bool parse_host_char()
    {
        // host-char = ALPHA / DIGIT / "-"
        return m_state.lexer.consume_specific_with_predicate(is_ascii_alpha)
            || m_state.lexer.consume_specific_with_predicate(is_ascii_digit)
            || m_state.lexer.consume_specific('-');
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-port-part
    [[nodiscard]] bool parse_port_part()
    {
        // port-part = 1*DIGIT / "*"
        StateTransaction transaction { *this };

        if (m_state.lexer.consume_specific('*')) {
            m_state.parse_result.port_part = transaction.parsed_string_view();
            transaction.commit();
            return true;
        }

        if (!m_state.lexer.consume_specific_with_predicate(is_ascii_digit))
            return false;

        (void)m_state.lexer.consume_while(is_ascii_digit);

        m_state.parse_result.port_part = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-path-part
    [[nodiscard]] bool parse_path_part()
    {
        // path-part = path-absolute (but not including ";" or ",")
        // ; path-absolute is defined in section 3.3 of RFC 3986.
        StateTransaction transaction { *this };

        if (!parse_path_absolute())
            return false;

        m_state.parse_result.path_part = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-3.3
    [[nodiscard]] bool parse_path_absolute()
    {
        // path-absolute = "/" [ segment-nz *( "/" segment ) ]
        if (!m_state.lexer.consume_specific('/'))
            return false;

        if (parse_segment_non_zero()) {
            while (m_state.lexer.consume_specific('/')) {
                parse_segment();
            }
        }

        return true;
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-3.3
    void parse_segment()
    {
        // segment = *pchar
        while (parse_path_character())
            ;
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-3.3
    [[nodiscard]] bool parse_segment_non_zero()
    {
        // segment-nz = 1*pchar
        if (!parse_path_character())
            return false;

        while (parse_path_character())
            ;

        return true;
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-3.3
    [[nodiscard]] bool parse_path_character()
    {
        // pchar = unreserved / pct-encoded / sub-delims / ":" / "@"
        return parse_unreserved()
            || parse_percent_encoded()
            || parse_sub_delims()
            || m_state.lexer.consume_specific_with_predicate(is_any_of(":@"sv));
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-2.3
    [[nodiscard]] bool parse_unreserved()
    {
        // unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"
        return m_state.lexer.consume_specific_with_predicate(is_ascii_alpha)
            || m_state.lexer.consume_specific_with_predicate(is_ascii_digit)
            || m_state.lexer.consume_specific_with_predicate(is_any_of("-._~"sv));
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-2.1
    [[nodiscard]] bool parse_percent_encoded()
    {
        // pct-encoded = "%" HEXDIG HEXDIG
        // "The uppercase hexadecimal digits 'A' through 'F' are equivalent to
        //  the lowercase digits 'a' through 'f', respectively.  If two URIs
        //  differ only in the case of hexadecimal digits used in percent-encoded
        //  octets, they are equivalent.  For consistency, URI producers and
        //  normalizers should use uppercase hexadecimal digits for all percent-
        //  encodings."
        return m_state.lexer.consume_specific('%')
            && m_state.lexer.consume_specific_with_predicate(is_ascii_hex_digit)
            && m_state.lexer.consume_specific_with_predicate(is_ascii_hex_digit);
    }

    // https://datatracker.ietf.org/doc/html/rfc3986#section-2.2
    [[nodiscard]] bool parse_sub_delims()
    {
        // sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
        //             / "*" / "+" / "," / ";" / "="
        // NOTE: This does not contain ';' and ',' as per the requirement specified in parse_path_part.
        return m_state.lexer.consume_specific_with_predicate(is_any_of("!$&'()*+="sv));
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-keyword-source
    [[nodiscard]] bool parse_keyword_source()
    {
        // ; Keywords:
        // keyword-source = "'self'" / "'unsafe-inline'" / "'unsafe-eval'"
        //                  / "'strict-dynamic'" / "'unsafe-hashes'" /
        //                  / "'report-sample'" / "'unsafe-allow-redirects'"
        //                  / "'wasm-unsafe-eval'"
        StateTransaction transaction { *this };

#define __ENUMERATE_KEYWORD_SOURCE(_, value)                                    \
    if (m_state.lexer.consume_specific(value##sv)) {                            \
        m_state.parse_result.keyword_source = transaction.parsed_string_view(); \
        transaction.commit();                                                   \
        return true;                                                            \
    }
        ENUMERATE_KEYWORD_SOURCES
#undef __ENUMERATE_KEYWORD_SOURCE

        return false;
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-nonce-source
    [[nodiscard]] bool parse_nonce_source()
    {
        // ; Nonces: 'nonce-[nonce goes here]'
        // nonce-source = "'nonce-" base64-value "'"
        auto prefix = m_state.lexer.consume(7);
        if (prefix.length() != 7)
            return false;

        if (!prefix.equals_ignoring_ascii_case("'nonce-"sv))
            return false;

        if (!parse_base64_value())
            return false;

        return m_state.lexer.consume_specific('\'');
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-base64-value
    [[nodiscard]] bool parse_base64_value()
    {
        // base64-value = 1*( ALPHA / DIGIT / "+" / "/" / "-" / "_" )*2( "=" )
        StateTransaction transaction { *this };

        auto is_main_part = [](char ch) {
            return is_ascii_alpha(ch) || is_ascii_digit(ch) || ch == '+' || ch == '/' || ch == '-' || ch == '_';
        };

        if (!m_state.lexer.consume_specific_with_predicate(is_main_part))
            return false;

        (void)m_state.lexer.consume_while(is_main_part);
        (void)m_state.lexer.consume_specific('=');
        (void)m_state.lexer.consume_specific('=');

        m_state.parse_result.base64_value = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-hash-source
    [[nodiscard]] bool parse_hash_source()
    {
        // ; Digests: 'sha256-[digest goes here]'
        // hash-source = "'" hash-algorithm "-" base64-value "'"
        if (!m_state.lexer.consume_specific('\''))
            return false;

        if (!parse_hash_algorithm())
            return false;

        if (!m_state.lexer.consume_specific('-'))
            return false;

        if (!parse_base64_value())
            return false;

        return m_state.lexer.consume_specific('\'');
    }

    // https://w3c.github.io/webappsec-csp/#grammardef-hash-algorithm
    [[nodiscard]] bool parse_hash_algorithm()
    {
        // hash-algorithm = "sha256" / "sha384" / "sha512"
        StateTransaction transaction { *this };

        auto hash_algorithm = m_state.lexer.consume(6);
        if (hash_algorithm.length() != 6)
            return false;

        if (hash_algorithm.equals_ignoring_ascii_case("sha256"sv)) {
            m_state.parse_result.hash_algorithm = transaction.parsed_string_view();
            transaction.commit();
            return true;
        }

        if (hash_algorithm.equals_ignoring_ascii_case("sha384"sv)) {
            m_state.parse_result.hash_algorithm = transaction.parsed_string_view();
            transaction.commit();
            return true;
        }

        if (hash_algorithm.equals_ignoring_ascii_case("sha512"sv)) {
            m_state.parse_result.hash_algorithm = transaction.parsed_string_view();
            transaction.commit();
            return true;
        }

        return false;
    }

private:
    struct State {
        GenericLexer lexer;
        SourceExpressionParseResult parse_result;
    };

    struct StateTransaction {
        explicit StateTransaction(SourceExpressionParser& parser)
            : m_parser(parser)
            , m_saved_state(parser.m_state)
            , m_start_index(parser.m_state.lexer.tell())
        {
        }

        ~StateTransaction()
        {
            if (!m_commit)
                m_parser.m_state = move(m_saved_state);
        }

        void commit() { m_commit = true; }
        StringView parsed_string_view() const
        {
            return m_parser.m_input.substring_view(m_start_index, m_parser.m_state.lexer.tell() - m_start_index);
        }

    private:
        SourceExpressionParser& m_parser;
        State m_saved_state;
        size_t m_start_index { 0 };
        bool m_commit { false };
    };

    StringView m_input;
    State m_state;
};

#define ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSERS                                   \
    __ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSER(SchemeSource, parse_scheme_source)   \
    __ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSER(HostSource, parse_host_source)       \
    __ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSER(KeywordSource, parse_keyword_source) \
    __ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSER(NonceSource, parse_nonce_source)     \
    __ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSER(HashSource, parse_hash_source)

Optional<SourceExpressionParseResult> parse_source_expression(Production production, StringView input)
{
    SourceExpressionParser parser { input };

    switch (production) {
#define __ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSER(ProductionName, parse_production) \
    case Production::ProductionName:                                                      \
        if (!parser.parse_production())                                                   \
            return {};                                                                    \
        break;
        ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSERS
#undef __ENUMERATE_SOURCE_EXPRESSION_PRODUCTION_PARSER
    default:
        VERIFY_NOT_REACHED();
    }

    // If we parsed successfully but didn't reach the end, the string doesn't match the given production.
    if (!parser.lexer().is_eof())
        return {};

    return parser.parse_result();
}

}
