/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibGC/Heap.h>
#include <LibHTTP/HTTP.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/ServerTiming/PerformanceServerTiming.h>

namespace Web::ServerTiming {

GC_DEFINE_ALLOCATOR(PerformanceServerTiming);

PerformanceServerTiming::PerformanceServerTiming(String metric_name)
    : m_metric_name(move(metric_name))
{
}

PerformanceServerTiming::~PerformanceServerTiming() = default;

// NB: Consumes a quoted-string, and returns its value with the quoted-pairs unescaped. Returns an empty Optional if
//     the quoted-string is not terminated.
static Optional<String> consume_quoted_string(GenericLexer& lexer)
{
    VERIFY(lexer.next_is('"'));
    lexer.ignore(1);

    StringBuilder builder;
    while (!lexer.is_eof()) {
        auto code_point = lexer.consume();
        if (code_point == '"') {
            lexer.ignore_while(HTTP::is_http_tab_or_space);
            // NB: A quoted-string can contain obs-text, which is not necessarily valid UTF-8.
            return String::from_utf8_with_replacement_character(builder.string_view(), String::WithBOMHandling::No);
        }
        if (code_point == '\\') {
            if (lexer.is_eof())
                return {};
            code_point = lexer.consume();
        }
        builder.append(code_point);
    }
    return {};
}

// https://w3c.github.io/server-timing/#dfn-parse-a-server-timing-header-field
// INTEROP: The parsing algorithm in the spec accepts metric names and parameter values that are not tokens, and matches
//          parameter names case-sensitively. Other engines and WPT instead follow the ABNF of the header field, which is
//          what we do here:
//
//          Server-Timing             = #server-timing-metric
//          server-timing-metric      = metric-name *( OWS ";" OWS server-timing-param )
//          metric-name               = token
//          server-timing-param       = server-timing-param-name OWS "=" OWS server-timing-param-value
//          server-timing-param-name  = token
//          server-timing-param-value = token / quoted-string
//
//          Parameter names are matched ASCII case-insensitively, only the first instance of each parameter is
//          considered, and extraneous characters after a metric name or a parameter value are ignored.
GC::Ptr<PerformanceServerTiming> PerformanceServerTiming::parse_a_server_timing_header_field(StringView field)
{
    GenericLexer lexer { field };

    auto consume_token = [&] {
        auto token = lexer.consume_while(HTTP::is_http_token_code_point);
        lexer.ignore_while(HTTP::is_http_tab_or_space);
        return token;
    };

    lexer.ignore_while(HTTP::is_http_tab_or_space);

    auto name = consume_token();
    if (name.is_empty())
        return nullptr;

    auto metric = GC::Heap::the().allocate<PerformanceServerTiming>(MUST(String::from_utf8(name)));

    lexer.ignore_until(';');

    while (lexer.consume_specific(';')) {
        lexer.ignore_while(HTTP::is_http_tab_or_space);

        auto param_name = consume_token();
        if (param_name.is_empty())
            break;

        String param_value;
        if (lexer.consume_specific('=')) {
            lexer.ignore_while(HTTP::is_http_tab_or_space);

            if (lexer.next_is('"')) {
                param_value = consume_quoted_string(lexer).value_or({});
            } else {
                param_value = MUST(String::from_utf8(consume_token()));
            }

            lexer.ignore_until(';');
        }

        auto lowercase_param_name = MUST(String::from_utf8(param_name)).to_ascii_lowercase();
        if (!metric->m_params.contains(lowercase_param_name))
            metric->m_params.set(move(lowercase_param_name), move(param_value));
    }

    return metric;
}

// https://w3c.github.io/server-timing/#dom-performanceservertiming-duration
HighResolutionTime::DOMHighResTimeStamp PerformanceServerTiming::duration() const
{
    // 1. If this’s params["dur"] does not exist, return 0.
    auto dur_param = m_params.get("dur"sv);
    if (!dur_param.has_value())
        return 0;

    // 2. Let dur be the result of parsing this’s params["dur"] using the rules for parsing floating-point number
    //    values.
    auto dur = HTML::parse_floating_point_number(Utf16String::from_utf8(*dur_param));

    // 3. If dur is an error, return 0; Otherwise return dur.
    return dur.value_or(0);
}

// https://w3c.github.io/server-timing/#dom-performanceservertiming-description
String PerformanceServerTiming::description() const
{
    // The description getter steps are to return this’s params["desc"] if it exists, otherwise the empty string.
    return m_params.get("desc"sv).value_or(String {});
}

}
