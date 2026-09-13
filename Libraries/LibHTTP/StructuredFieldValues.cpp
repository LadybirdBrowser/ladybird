/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/StringBuilder.h>
#include <LibHTTP/StructuredFieldValues.h>

namespace HTTP::StructuredFieldValues {

Optional<BareItem const&> Parameters::get(StringView key) const
{
    for (auto const& entry : entries) {
        if (entry.key == key)
            return entry.value;
    }
    return {};
}

void Parameters::set(String key, BareItem value)
{
    for (auto& entry : entries) {
        if (entry.key == key) {
            entry.value = move(value);
            return;
        }
    }
    entries.append({ move(key), move(value) });
}

Optional<StringView> Item::token() const
{
    if (auto const* token = value.get_pointer<Token>())
        return token->value.bytes_as_string_view();
    return {};
}

// https://www.rfc-editor.org/rfc/rfc8941#section-3.3.3
static bool is_tchar(char c)
{
    // tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
    return is_ascii_alphanumeric(c) || "!#$%&'*+-.^_`|~"sv.contains(c);
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.4
static Optional<BareItem> parse_integer_or_decimal(GenericLexer& lexer)
{
    // 1. Let type be "integer".
    bool is_decimal = false;

    // 2. Let sign be 1.
    i64 sign = 1;

    // 3. Let input_number be an empty string.
    StringBuilder input_number;

    // 4. If the first character of input_string is "-", consume it and set sign to -1.
    if (lexer.consume_specific('-'))
        sign = -1;

    // 5. If input_string is empty, there is an empty integer; fail parsing.
    // 6. If the first character of input_string is not a DIGIT, fail parsing.
    if (lexer.is_eof() || !is_ascii_digit(lexer.peek()))
        return {};

    // 7. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto c = lexer.consume();

        // 2. If char is a DIGIT, append it to input_number.
        if (is_ascii_digit(c)) {
            input_number.append(c);
        }
        // 3. Else, if type is "integer" and char is ".":
        else if (!is_decimal && c == '.') {
            // 1. If input_number contains more than 12 characters, fail parsing.
            if (input_number.length() > 12)
                return {};

            // 2. Otherwise, append char to input_number and set type to "decimal".
            input_number.append(c);
            is_decimal = true;
        }
        // 4. Otherwise, prepend char to input_string, and exit the loop.
        else {
            lexer.retreat();
            break;
        }

        // 5. If type is "integer" and input_number contains more than 15 characters, fail parsing.
        if (!is_decimal && input_number.length() > 15)
            return {};

        // 6. If type is "decimal" and input_number contains more than 16 characters, fail parsing.
        if (is_decimal && input_number.length() > 16)
            return {};
    }

    auto number = input_number.string_view();

    // 8. If type is "integer":
    if (!is_decimal) {
        // 1. Parse input_number as an integer and let output_number be the product of the result and sign.
        auto value = number.to_number<i64>();
        if (!value.has_value())
            return {};
        return BareItem { sign * *value };
    }

    // 9. Otherwise:
    // 1. If the final character of input_number is ".", fail parsing.
    if (number.ends_with('.'))
        return {};

    // 2. If the number of characters after "." in input_number is greater than three, fail parsing.
    auto fraction_length = number.length() - number.find('.').value() - 1;
    if (fraction_length > 3)
        return {};

    // 3. Parse input_number as a decimal number and let output_number be the product of the result and sign.
    auto value = number.to_number<double>();
    if (!value.has_value())
        return {};
    return BareItem { static_cast<double>(sign) * *value };
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.5
static Optional<BareItem> parse_string(GenericLexer& lexer)
{
    // 1. Let output_string be an empty string.
    StringBuilder output_string;

    // 2. If the first character of input_string is not DQUOTE, fail parsing.
    // 3. Discard the first character of input_string.
    if (!lexer.consume_specific('"'))
        return {};

    // 4. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto c = lexer.consume();

        // 2. If char is a backslash ("\"):
        if (c == '\\') {
            // 1. If input_string is now empty, fail parsing.
            if (lexer.is_eof())
                return {};

            // 2. Let next_char be the result of consuming the first character of input_string.
            auto next_char = lexer.consume();

            // 3. If next_char is not DQUOTE or "\", fail parsing.
            if (next_char != '"' && next_char != '\\')
                return {};

            // 4. Append next_char to output_string.
            output_string.append(next_char);
        }
        // 3. Else, if char is DQUOTE, return output_string.
        else if (c == '"') {
            return BareItem { MUST(output_string.to_string()) };
        }
        // 4. Else, if char is in the range %x00-1f or %x7f-ff (i.e., it is not in VCHAR or SP), fail parsing.
        else if (static_cast<u8>(c) <= 0x1f || static_cast<u8>(c) >= 0x7f) {
            return {};
        }
        // 5. Else, append char to output_string.
        else {
            output_string.append(c);
        }
    }

    // 5. Reached the end of input_string without finding a closing DQUOTE; fail parsing.
    return {};
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.6
static Optional<BareItem> parse_token(GenericLexer& lexer)
{
    // 1. If the first character of input_string is not ALPHA or "*", fail parsing.
    if (lexer.is_eof() || !(is_ascii_alpha(lexer.peek()) || lexer.peek() == '*'))
        return {};

    // 2. Let output_string be an empty string.
    // 3. While input_string is not empty:
    //    1. If the first character of input_string is not in tchar, ":", or "/", return output_string.
    //    2. Let char be the result of consuming the first character of input_string.
    //    3. Append char to output_string.
    auto output_string = lexer.consume_while([](char c) { return is_tchar(c) || c == ':' || c == '/'; });

    // 4. Return output_string.
    return BareItem { Token { MUST(String::from_utf8(output_string)) } };
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.7
static Optional<BareItem> parse_byte_sequence(GenericLexer& lexer)
{
    // 1. If the first character of input_string is not ":", fail parsing.
    // 2. Discard the first character of input_string.
    if (!lexer.consume_specific(':'))
        return {};

    // 3. If there is not a ":" character before the end of input_string, fail parsing.
    // 4. Let b64_content be the result of consuming content of input_string up to but not including the first
    //    instance of the character ":".
    auto b64_content = lexer.consume_until(':');
    if (lexer.is_eof())
        return {};

    // 5. Consume the ":" character at the beginning of input_string.
    lexer.ignore();

    // 6. If b64_content contains a character not included in ALPHA, DIGIT, "+", "/", and "=", fail parsing.
    for (auto c : b64_content) {
        if (!(is_ascii_alphanumeric(c) || c == '+' || c == '/' || c == '='))
            return {};
    }

    // 7. Let binary_content be the result of base64-decoding b64_content, synthesizing padding if necessary
    //    (note the requirements about recipient behavior below). If base64 decoding fails, parsing fails.
    auto binary_content = decode_base64(b64_content);
    if (binary_content.is_error())
        return {};

    // 8. Return binary_content.
    return BareItem { ByteSequence { binary_content.release_value() } };
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.8
static Optional<BareItem> parse_boolean(GenericLexer& lexer)
{
    // 1. If the first character of input_string is not "?", fail parsing.
    // 2. Discard the first character of input_string.
    if (!lexer.consume_specific('?'))
        return {};

    // 3. If the first character of input_string matches "1", discard the first character, and return true.
    if (lexer.consume_specific('1'))
        return BareItem { true };

    // 4. If the first character of input_string matches "0", discard the first character, and return false.
    if (lexer.consume_specific('0'))
        return BareItem { false };

    // 5. No value has matched; fail parsing.
    return {};
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.3.1
static Optional<BareItem> parse_bare_item(GenericLexer& lexer)
{
    if (lexer.is_eof())
        return {};
    auto c = lexer.peek();

    // 1. If the first character of input_string is a "-" or a DIGIT, return the result of running Parsing an Integer
    //    or Decimal with input_string.
    if (c == '-' || is_ascii_digit(c))
        return parse_integer_or_decimal(lexer);

    // 2. If the first character of input_string is a DQUOTE, return the result of running Parsing a String with
    //    input_string.
    if (c == '"')
        return parse_string(lexer);

    // 3. If the first character of input_string is an ALPHA or "*", return the result of running Parsing a Token
    //    with input_string.
    if (is_ascii_alpha(c) || c == '*')
        return parse_token(lexer);

    // 4. If the first character of input_string is ":", return the result of running Parsing a Byte Sequence with
    //    input_string.
    if (c == ':')
        return parse_byte_sequence(lexer);

    // 5. If the first character of input_string is "?", return the result of running Parsing a Boolean with
    //    input_string.
    if (c == '?')
        return parse_boolean(lexer);

    // 6. Otherwise, the item type is unrecognized; fail parsing.
    return {};
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.3.3
static Optional<String> parse_key(GenericLexer& lexer)
{
    // 1. If the first character of input_string is not lcalpha or "*", fail parsing.
    if (lexer.is_eof() || !(is_ascii_lower_alpha(lexer.peek()) || lexer.peek() == '*'))
        return {};

    // 2. Let output_string be an empty string.
    // 3. While input_string is not empty:
    //    1. If the first character of input_string is not one of lcalpha, DIGIT, "_", "-", ".", or "*", return
    //       output_string.
    //    2. Let char be the result of consuming the first character of input_string.
    //    3. Append char to output_string.
    auto output_string = lexer.consume_while([](char c) { return is_ascii_lower_alpha(c) || is_ascii_digit(c) || c == '_' || c == '-' || c == '.' || c == '*'; });

    // 4. Return output_string.
    return MUST(String::from_utf8(output_string));
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2.3.2
static Optional<Parameters> parse_parameters(GenericLexer& lexer)
{
    // 1. Let parameters be an empty, ordered map.
    Parameters parameters;

    // 2. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. If the first character of input_string is not ";", exit the loop.
        // 2. Consume the ";" character from the beginning of input_string.
        if (!lexer.consume_specific(';'))
            break;

        // 3. Discard any leading SP characters from input_string.
        lexer.ignore_while([](char c) { return c == ' '; });

        // 4. Let param_key be the result of running Parsing a Key with input_string.
        auto param_key = parse_key(lexer);
        if (!param_key.has_value())
            return {};

        // 5. Let param_value be Boolean true.
        BareItem param_value { true };

        // 6. If the first character of input_string is "=":
        if (lexer.consume_specific('=')) {
            // 1. Consume the "=" character at the beginning of input_string.
            // 2. Let param_value be the result of running Parsing a Bare Item with input_string.
            auto parsed_value = parse_bare_item(lexer);
            if (!parsed_value.has_value())
                return {};
            param_value = parsed_value.release_value();
        }

        // 7. If parameters already contains a key param_key (comparing character for character), overwrite its value
        //    with param_value.
        // 8. Otherwise, append key param_key with value param_value to parameters.
        parameters.set(param_key.release_value(), move(param_value));
    }

    // 3. Return parameters.
    return parameters;
}

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2
Optional<Item> parse_item(StringView input)
{
    // 1. Convert input_bytes into an ASCII string input_string; if conversion fails, fail parsing.
    for (auto c : input) {
        if (static_cast<u8>(c) > 0x7f)
            return {};
    }
    GenericLexer lexer { input };

    // 2. Discard any leading SP characters from input_string.
    lexer.ignore_while([](char c) { return c == ' '; });

    // 5. If field_type is "item", let output be the result of running Parsing an Item (Section 4.2.3) with
    //    input_string.
    // https://www.rfc-editor.org/rfc/rfc8941#section-4.2.3
    // 1. Let bare_item be the result of running Parsing a Bare Item (Section 4.2.3.1) with input_string.
    auto bare_item = parse_bare_item(lexer);
    if (!bare_item.has_value())
        return {};

    // 2. Let parameters be the result of running Parsing Parameters (Section 4.2.3.2) with input_string.
    auto parameters = parse_parameters(lexer);
    if (!parameters.has_value())
        return {};

    // 6. Discard any leading SP characters from input_string.
    lexer.ignore_while([](char c) { return c == ' '; });

    // 7. If input_string is not empty, fail parsing.
    if (!lexer.is_eof())
        return {};

    // 8. Otherwise, return output.
    return Item { bare_item.release_value(), parameters.release_value() };
}

}
