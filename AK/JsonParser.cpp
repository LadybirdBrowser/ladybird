/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/FloatingPointStringConversions.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <math.h>

namespace AK {

constexpr bool is_space(int ch)
{
    return ch == '\t' || ch == '\n' || ch == '\r' || ch == ' ';
}

ErrorOr<JsonValue> JsonParser::parse(StringView input)
{
    JsonParser parser(input);
    return parser.parse_json();
}

// ECMA-404 9 String
// Boils down to
// STRING = "\"" *("[^\"\\]" | "\\" ("[\"\\bfnrt]" | "u[0-9A-Za-z]{4}")) "\""
//     │├── " ──╮───────────────────────────────────────────────╭── " ──┤│
//              │                                               │
//              │  ╭───────────────────<─────────────────────╮  │
//              │  │                                         │  │
//              ╰──╰──╮───────────── [^"\\] ──────────────╭──╯──╯
//                    │                                   │
//                    ╰── \ ───╮──── ["\\bfnrt] ───────╭──╯
//                             │                       │
//                             ╰─── u[0-9A-Za-z]{4}  ──╯
//
ErrorOr<ByteString> JsonParser::consume_and_unescape_string()
{
    if (!consume_specific('"'))
        return Error::from_string_literal("JsonParser: Expected '\"'");
    StringBuilder final_sb;

    for (;;) {
        // OPTIMIZATION: We try to append as many literal characters as possible at a time
        //               This also pre-checks some error conditions
        // Note: All utf8 characters are either plain ascii,  or have their most signifiant bit set,
        //       which puts the, above plain ascii in value, so they will always consist
        //       of a set of "legal" non-special bytes,
        //       hence we don't need to bother with a code-point iterator,
        //       as a simple byte iterator suffices, which GenericLexer provides by default
        size_t literal_characters = 0;
        for (;;) {
            char ch = peek(literal_characters);
            // Note: We get a 0 byte when we hit EOF
            if (ch == 0)
                return Error::from_string_literal("JsonParser: EOF while parsing String");
            // Spec: All code points may be placed within the quotation marks except
            //       for the code points that must be escaped: quotation mark (U+0022),
            //       reverse solidus (U+005C), and the control characters U+0000 to U+001F.
            //       There are two-character escape sequence representations of some characters.
            if (is_ascii_c0_control(ch))
                return Error::from_string_literal("JsonParser: ASCII control sequence encountered");
            if (ch == '"' || ch == '\\')
                break;
            ++literal_characters;
        }
        final_sb.append(consume(literal_characters));

        // We have checked all cases except end-of-string and escaped characters in the loop above,
        // so we now only have to handle those two cases
        char ch = peek();

        if (ch == '"') {
            consume();
            break;
        }

        ignore(); // '\'

        switch (peek()) {
        case '\0':
            return Error::from_string_literal("JsonParser: EOF while parsing String");
        case '"':
        case '\\':
        case '/':
            final_sb.append(consume());
            break;
        case 'b':
            ignore();
            final_sb.append('\b');
            break;
        case 'f':
            ignore();
            final_sb.append('\f');
            break;
        case 'n':
            ignore();
            final_sb.append('\n');
            break;
        case 'r':
            ignore();
            final_sb.append('\r');
            break;
        case 't':
            ignore();
            final_sb.append('\t');
            break;
        case 'u': {
            ignore(); // 'u'

            // https://ecma-international.org/wp-content/uploads/ECMA-404_2nd_edition_december_2017.pdf
            //
            // To escape a code point that is not in the Basic Multilingual Plane, the character may be represented as a
            // twelve-character sequence, encoding the UTF-16 surrogate pair corresponding to the code point. So for
            // example, a string containing only the G clef character (U+1D11E) may be represented as "\uD834\uDD1E".
            // However, whether a processor of JSON texts interprets such a surrogate pair as a single code point or as an
            // explicit surrogate pair is a semantic decision that is determined by the specific processor.
            auto code_point = decode_single_or_paired_surrogate();

            if (code_point.is_error())
                return Error::from_string_literal("JsonParser: Error while parsing Unicode escape");

            final_sb.append_code_point(code_point.value());
            break;
        }
        default:
            dbgln("JsonParser: Invalid escaped character '{}' ({:#x}) ", peek(), peek());
            return Error::from_string_literal("JsonParser: Invalid escaped character");
        }
    }

    return final_sb.to_byte_string();
}

ErrorOr<JsonValue> JsonParser::parse_object()
{
    JsonObject object;
    if (!consume_specific('{'))
        return Error::from_string_literal("JsonParser: Expected '{'");
    for (;;) {
        ignore_while(is_space);
        if (peek() == '}')
            break;
        ignore_while(is_space);
        auto name = TRY(consume_and_unescape_string());
        ignore_while(is_space);
        if (!consume_specific(':'))
            return Error::from_string_literal("JsonParser: Expected ':'");
        ignore_while(is_space);
        auto value = TRY(parse_helper());
        object.set(name, move(value));
        ignore_while(is_space);
        if (peek() == '}')
            break;
        if (!consume_specific(','))
            return Error::from_string_literal("JsonParser: Expected ','");
        ignore_while(is_space);
        if (peek() == '}')
            return Error::from_string_literal("JsonParser: Unexpected '}'");
    }
    if (!consume_specific('}'))
        return Error::from_string_literal("JsonParser: Expected '}'");
    return JsonValue { move(object) };
}

ErrorOr<JsonValue> JsonParser::parse_array()
{
    JsonArray array;
    if (!consume_specific('['))
        return Error::from_string_literal("JsonParser: Expected '['");
    for (;;) {
        ignore_while(is_space);
        if (peek() == ']')
            break;
        auto element = TRY(parse_helper());
        TRY(array.append(move(element)));
        ignore_while(is_space);
        if (peek() == ']')
            break;
        if (!consume_specific(','))
            return Error::from_string_literal("JsonParser: Expected ','");
        ignore_while(is_space);
        if (peek() == ']')
            return Error::from_string_literal("JsonParser: Unexpected ']'");
    }
    ignore_while(is_space);
    if (!consume_specific(']'))
        return Error::from_string_literal("JsonParser: Expected ']'");
    return JsonValue { move(array) };
}

ErrorOr<JsonValue> JsonParser::parse_string()
{
    auto string = TRY(consume_and_unescape_string());
    return JsonValue(move(string));
}

ErrorOr<JsonValue> JsonParser::parse_number()
{
    Vector<char, 32> number_buffer;

    auto start_index = tell();

    bool negative = false;
    if (peek() == '-') {
        number_buffer.append('-');
        ++m_index;
        negative = true;

        if (!is_ascii_digit(peek()))
            return Error::from_string_literal("JsonParser: Unexpected '-' without further digits");
    }

    auto fallback_to_double_parse = [&]() -> ErrorOr<JsonValue> {
        // FIXME: Since we know all the characters so far are ascii digits (and one . or e) we could
        //        use that in the floating point parser.

        // The first part should be just ascii digits
        StringView view = m_input.substring_view(start_index);

        char const* start = view.characters_without_null_termination();
        auto parse_result = parse_first_floating_point(start, start + view.length());

        if (parse_result.parsed_value()) {
            auto characters_parsed = parse_result.end_ptr - start;
            m_index = start_index + characters_parsed;

            return JsonValue(parse_result.value);
        }
        return Error::from_string_literal("JsonParser: Invalid floating point");
    };

    if (peek() == '0') {
        if (is_ascii_digit(peek(1)))
            return Error::from_string_literal("JsonParser: Cannot have leading zeros");

        // Leading zeros are not allowed, however we can have a '.' or 'e' with
        // valid digits after just a zero. These cases will be detected by having the next element
        // start with a '.' or 'e'.
    }

    bool all_zero = true;
    for (;;) {
        char ch = peek();
        if (ch == '.') {
            if (!is_ascii_digit(peek(1)))
                return Error::from_string_literal("JsonParser: Must have digits after decimal point");

            return fallback_to_double_parse();
        }
        if (ch == 'e' || ch == 'E') {
            char next = peek(1);
            if (!is_ascii_digit(next) && ((next != '+' && next != '-') || !is_ascii_digit(peek(2))))
                return Error::from_string_literal("JsonParser: Must have digits after exponent with an optional sign inbetween");

            return fallback_to_double_parse();
        }

        if (is_ascii_digit(ch)) {
            if (ch != '0')
                all_zero = false;

            number_buffer.append(ch);
            ++m_index;
            continue;
        }

        break;
    }

    // Negative zero is always a double
    if (negative && all_zero)
        return JsonValue(-0.0);

    StringView number_string(number_buffer.data(), number_buffer.size());

    if (auto number = number_string.to_number<u64>(); number.has_value())
        return JsonValue(*number);
    if (auto number = number_string.to_number<i64>(); number.has_value())
        return JsonValue(*number);

    // It's possible the unsigned value is bigger than u64 max
    return fallback_to_double_parse();
}

ErrorOr<JsonValue> JsonParser::parse_true()
{
    if (!consume_specific("true"sv))
        return Error::from_string_literal("JsonParser: Expected 'true'");
    return JsonValue(true);
}

ErrorOr<JsonValue> JsonParser::parse_false()
{
    if (!consume_specific("false"sv))
        return Error::from_string_literal("JsonParser: Expected 'false'");
    return JsonValue(false);
}

ErrorOr<JsonValue> JsonParser::parse_null()
{
    if (!consume_specific("null"sv))
        return Error::from_string_literal("JsonParser: Expected 'null'");
    return JsonValue {};
}

ErrorOr<JsonValue> JsonParser::parse_helper()
{
    ignore_while(is_space);
    auto type_hint = peek();
    switch (type_hint) {
    case '{':
        return parse_object();
    case '[':
        return parse_array();
    case '"':
        return parse_string();
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        return parse_number();
    case 'f':
        return parse_false();
    case 't':
        return parse_true();
    case 'n':
        return parse_null();
    }

    return Error::from_string_literal("JsonParser: Unexpected character");
}

ErrorOr<JsonValue> JsonParser::parse_json()
{
    auto result = TRY(parse_helper());
    ignore_while(is_space);
    if (!is_eof())
        return Error::from_string_literal("JsonParser: Didn't consume all input");
    return result;
}

}
