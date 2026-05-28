/*
 * Copyright (c) 2026, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/GenericLexer.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringConversions.h>
#include <AK/Try.h>
#include <AK/Tuple.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibHTTP/StructuredField.h>

namespace HTTP {

static constexpr bool is_sp_character(char c)
{
    return c == 0x20;
}

// https://www.rfc-editor.org/info/rfc9110/#name-whitespace
static constexpr bool is_ows_character(char c)
{
    return c == 0x20 || c == 0x09;
}

// https://www.rfc-editor.org/info/rfc9110/#name-tokens
static constexpr bool is_tchar(char c)
{
    return c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*'
        || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~'
        || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0x21 && c <= 0x7e && c != '"' && c != '(' && c != ')' && c != ',' && c != '/' && c != ':' && c != ';' && c != '<' && c != '=' && c != '>' && c != '?' && c != '@' && c != '[' && c != '\\' && c != ']' && c != '{' && c != '}');
}

enum class NumberType : u8 {
    Integer,
    Decimal
};

// https://httpwg.org/specs/rfc9651.html#parse-number
static ErrorOr<Variant<StructuredFieldInteger, StructuredFieldDecimal>> parse_an_integer_or_decimal(GenericLexer lexer)
{
    // 1. Let type be "integer".
    auto type = NumberType::Integer;

    // 2. Let sign be 1.
    auto sign = 1;

    // 3. Let input_number be an empty string.
    StringBuilder input_number {};

    // 4. If the first character of input_string is "-", consume it and set sign to -1.
    if (lexer.peek() == '-') {
        lexer.consume();
        sign = -1;
    }

    // 5. If input_string is empty, there is an empty integer; fail parsing.
    if (lexer.is_eof())
        return Error::from_string_view("A number cannot be empty"sv);

    // 6. If the first character of input_string is not a DIGIT, fail parsing.
    auto first_character = lexer.peek();
    if (first_character < '0' || first_character > '9')
        return Error::from_string_view("A number must start with a digit"sv);

    // 7. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto c = lexer.consume();

        // 2. If char is a DIGIT, append it to input_number.
        if (c >= '0' && c <= '9') {
            input_number.append(c);
        }

        // 3. Else, if type is "integer" and char is ".":
        else if (type == NumberType::Integer && c == '.') {
            // 1. If input_number contains more than 12 characters, fail parsing.
            if (input_number.length() > 12)
                return Error::from_string_view("A number cannot contain more than 12 digits"sv);

            // 2. Otherwise, append char to input_number and set type to "decimal".
            input_number.append(c);
            type = NumberType::Decimal;
        }

        // 4. Otherwise, prepend char to input_string, and exit the loop.
        else {
            lexer.retreat();
            break;
        }
    }

    // 5. If type is "integer" and input_number contains more than 15 characters, fail parsing.
    if (type == NumberType::Integer && input_number.length() > 15)
        return Error::from_string_view("An integer cannot contain more than 15 characters"sv);

    // 6. If type is "decimal" and input_number contains more than 16 characters, fail parsing.
    if (type == NumberType::Decimal && input_number.length() > 16)
        return Error::from_string_view("A double cannot contain more than 16 characters"sv);

    // 8. If type is "integer":
    double output_number = 0;
    if (type == NumberType::Integer) {
        // 1. Let output_number be an Integer that is the result of parsing input_number as an integer.
        auto parsed_number = AK::parse_number<i64>(input_number.string_view());
        if (!parsed_number.has_value())
            return Error::from_string_view("Unable to parse integer"sv);
        output_number = (double)parsed_number.value();
    }

    // 9. Otherwise:
    else {
        // 1. If the final character of input_number is ".", fail parsing.
        auto dot_index = input_number.string_view().find_last('.').value();
        if (dot_index == input_number.length() - 1)
            return Error::from_string_view("The dot cannot be the last character of a double"sv);

        // 2. If the number of characters after "." in input_number is greater than three, fail parsing.
        if (dot_index >= input_number.length() - 4)
            return Error::from_string_view("There can at most be 3 digits after the dot in a double"sv);

        // 3. Let output_number be a Decimal that is the result of parsing input_number as a decimal number.
        auto parsed_number = AK::parse_number<double>(input_number.string_view());
        if (!parsed_number.has_value())
            return Error::from_string_view("Unable to parse double"sv);
        output_number = parsed_number.value();
    }

    // 10. Let output_number be the product of output_number and sign.
    output_number = sign * output_number;

    // 11. Return output_number.
    if (type == NumberType::Integer) {
        return StructuredFieldInteger { (i64)output_number };
    }
    return StructuredFieldDecimal { output_number };
}

// https://httpwg.org/specs/rfc9651.html#parse-string
static ErrorOr<StructuredFieldString> parse_a_string(GenericLexer lexer)
{
    // 1. Let output_string be an empty string.
    StringBuilder output_string {};

    // 2. If the first character of input_string is not DQUOTE, fail parsing.
    if (lexer.peek() != '"')
        return Error::from_string_view("A string must start with a double quote"sv);

    // 3. Discard the first character of input_string.
    lexer.consume();

    // 4. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto c = lexer.consume();

        // 2. If char is a backslash ("\"):
        if (c == '\\') {
            // 1. If input_string is now empty, fail parsing.
            if (lexer.is_eof())
                return Error::from_string_view("Unterminated escape sequence"sv);

            // 2. Let next_char be the result of consuming the first character of input_string.
            char escaped = lexer.consume();

            // 3. If next_char is not DQUOTE or "\", fail parsing.
            if (escaped != '"' && escaped != '\\')
                return Error::from_string_view("Invalid escape sequence"sv);

            // 4. Append next_char to output_string.
            output_string.append(escaped);
        }

        // 3. Else, if char is DQUOTE, return output_string.
        else if (c == '"') {
            return StructuredFieldString { TRY(output_string.to_string()) };
        }

        // 4. Else, if char is in the range %x00-1f or %x7f-ff (i.e., it is not in VCHAR or SP), fail parsing.
        else if (c <= 0x1f || c >= 0x7f) {
            return Error::from_string_view("String contains invalid characters"sv);
        }

        // 5. Else, append char to output_string.
        else {
            output_string.append(c);
        }
    }

    // 5. Reached the end of input_string without finding a closing DQUOTE; fail parsing.
    return Error::from_string_view("Unterminated string"sv);
}

// https://httpwg.org/specs/rfc9651.html#parse-token
static ErrorOr<StructuredFieldToken> parse_a_token(GenericLexer lexer)
{
    // 1. If the first character of input_string is not ALPHA or "*", fail parsing.
    auto first_character = lexer.peek();
    if ((first_character < 'a' || first_character > 'z') && (first_character < 'A' || first_character > 'Z') && first_character != '*')
        return Error::from_string_view("A token must start with a alphanumeric character or star"sv);

    // 2. Let output_string be an empty string.
    // 3. While input_string is not empty:
    //     1. If the first character of input_string is not in tchar, ":", or "/", return output_string.
    //     2. Let char be the result of consuming the first character of input_string.
    //     3. Append char to output_string.
    // 4. Return output_string.
    auto output = lexer.consume_until([](char c) { return !is_tchar(c) && c != ':' && c != '/'; });
    return StructuredFieldToken { TRY(String::from_utf8(output)) };
}

// https://httpwg.org/specs/rfc9651.html#parse-binary
static ErrorOr<StructuredFieldByteSequence> parse_a_byte_sequence(GenericLexer lexer)
{
    // 1. If the first character of input_string is not ":", fail parsing.
    if (lexer.peek() != ':')
        return Error::from_string_view("A byte sequence must start with a colon"sv);

    // 2. Discard the first character of input_string.
    lexer.consume();

    // 3. If there is not a ":" character before the end of input_string, fail parsing.
    if (!lexer.remaining().contains(':'))
        return Error::from_string_view("There must be a colon before the end of the input"sv);

    // 4. Let b64_content be the result of consuming content of input_string up to but not including the first instance of the character ":".
    auto b64_content = lexer.consume_until(':');

    // 5. Consume the ":" character at the beginning of input_string.
    lexer.consume();

    // 6. If b64_content contains a character not included in ALPHA, DIGIT, "+", "/", and "=", fail parsing.
    // 7. Let binary_content be the result of base64-decoding [RFC4648] b64_content, synthesizing padding if necessary (note the requirements about recipient behavior below). If base64 decoding fails, parsing fails.
    auto binary_content = TRY(decode_base64(b64_content));

    // 8. Return binary_content.
    return StructuredFieldByteSequence { binary_content };
}

// https://httpwg.org/specs/rfc9651.html#parse-boolean
static ErrorOr<StructuredFieldBoolean> parse_a_boolean(GenericLexer lexer)
{
    // 1. If the first character of input_string is not "?", fail parsing.
    if (lexer.peek() != '?')
        return Error::from_string_view("A boolean must start with a question mark"sv);

    // 2. Discard the first character of input_string.
    lexer.consume();

    // 3. If the first character of input_string matches "1", discard the first character, and return true.
    auto first_character = lexer.peek();
    if (first_character == '1') {
        lexer.consume();
        return StructuredFieldBoolean { true };
    }

    // 4. If the first character of input_string matches "0", discard the first character, and return false.
    if (first_character == '0') {
        lexer.consume();
        return StructuredFieldBoolean { false };
    }

    // 5. No value has matched; fail parsing.
    return Error::from_string_view("Invalid boolean value"sv);
}

// https://httpwg.org/specs/rfc9651.html#parse-date
static ErrorOr<StructuredFieldDate> parse_a_date(GenericLexer lexer)
{
    // 1. If the first character of input_string is not "@", fail parsing.
    if (lexer.peek() != '@')
        return Error::from_string_view("A boolean must start with an at sign"sv);

    // 2. Discard the first character of input_string.
    lexer.consume();

    // 3. Let output_date be the result of running Parsing an Integer or Decimal (Section 4.2.4) with input_string.
    auto output_date = TRY(parse_an_integer_or_decimal(lexer));

    // 4. If output_date is a Decimal, fail parsing.
    if (!output_date.has<StructuredFieldInteger>())
        return Error::from_string_view("The date must have an integer value"sv);

    // 5. Return output_date.
    return StructuredFieldDate { output_date.get<StructuredFieldInteger>().value };
}

// https://httpwg.org/specs/rfc9651.html#parse-display
static ErrorOr<StructuredFieldDisplayString> parse_a_display_string(GenericLexer lexer)
{
    // 1. If the first two characters of input_string are not "%" followed by DQUOTE, fail parsing.
    if (lexer.peek_string(2) != "%\""sv)
        return Error::from_string_view("A boolean must start with an empercent and a double quote"sv);

    // 2. Discard the first two characters of input_string.
    lexer.consume(2);

    // 3. Let byte_array be an empty byte array.
    ByteBuffer byte_array;

    // 4. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto c = lexer.consume();

        // 2. If char is in the range %x00-1f or %x7f-ff (i.e., it is not in VCHAR or SP), fail parsing.
        if (c <= 0x1f || c >= 0x7f)
            return Error::from_string_view("Display string contains invalid character"sv);

        // 3. If char is "%":
        if (c == '%') {
            // 1. Let octet_hex be the result of consuming two characters from input_string. If there are not two characters, fail parsing.
            auto octet_hex = lexer.consume(2);
            if (octet_hex.length() < 2)
                return Error::from_string_view("Unterminated hex octet in display string"sv);

            // 2. If octet_hex contains characters outside the range %x30-39 or %x61-66 (i.e., it is not in 0-9 or lowercase a-f), fail parsing.
            if (!all_of(octet_hex, is_ascii_hex_digit))
                return Error::from_string_view("Invalid character in hex octet"sv);

            // 3. Let octet be the result of hex decoding octet_hex (Section 8 of [RFC4648]).
            auto octet = AK::parse_hexadecimal_number<u8>(octet_hex);
            if (!octet.has_value())
                return Error::from_string_view("Invalid hex octet"sv);

            // 4. Append octet to byte_array.
            byte_array.append(octet.value());
        }

        // 4. If char is DQUOTE:
        else if (c == '"') {
            // 1. Let unicode_sequence be the result of decoding byte_array as a UTF-8 string (Section 3 of [UTF8]). Fail parsing if decoding fails.
            auto unicode_sequence = TRY(String::from_utf8(byte_array));

            // 2. Return unicode_sequence.
            return StructuredFieldDisplayString { unicode_sequence };
        }

        // 5. Otherwise, if char is not "%" or DQUOTE:
        else {
            // 1. Let byte be the result of applying ASCII encoding to char.
            u8 byte = c;

            // 2. Append byte to byte_array.
            byte_array.append(byte);
        }
    }

    // 5. Reached the end of input_string without finding a closing DQUOTE; fail parsing.
    return Error::from_string_view("Reached the end of a display string without finding a closing double quote"sv);
}

// https://httpwg.org/specs/rfc9651.html#parse-bare-item
static ErrorOr<StructuredFieldBareItem> parse_a_bare_item(GenericLexer lexer)
{
    // 1. If the first character of input_string is a "-" or a DIGIT, return the result of running Parsing an Integer or Decimal (Section 4.2.4) with input_string.
    auto first_character = lexer.peek();
    if (first_character == '-' || (first_character >= '0' && first_character <= '9')) {
        return parse_an_integer_or_decimal(lexer);
    }

    // 2. If the first character of input_string is a DQUOTE, return the result of running Parsing a String (Section 4.2.5) with input_string.
    if (first_character == '"') {
        return parse_a_string(lexer);
    }

    // 3. If the first character of input_string is an ALPHA or "*", return the result of running Parsing a Token (Section 4.2.6) with input_string.
    if ((first_character >= 'a' && first_character <= 'z') || (first_character >= 'A' && first_character <= 'Z') || first_character == '*') {
        return parse_a_token(lexer);
    }

    // 4. If the first character of input_string is ":", return the result of running Parsing a Byte Sequence (Section 4.2.7) with input_string.
    if (first_character == ':') {
        return parse_a_byte_sequence(lexer);
    }

    // 5. If the first character of input_string is "?", return the result of running Parsing a Boolean (Section 4.2.8) with input_string.
    if (first_character == '?') {
        return parse_a_boolean(lexer);
    }

    // 6. If the first character of input_string is "@", return the result of running Parsing a Date (Section 4.2.9) with input_string.
    if (first_character == '@') {
        return parse_a_date(lexer);
    }

    // 7. If the first character of input_string is "%", return the result of running Parsing a Display String (Section 4.2.10) with input_string.
    if (first_character == '%') {
        return parse_a_display_string(lexer);
    }

    // 8. Otherwise, the item type is unrecognized; fail parsing.
    return Error::from_string_view("Could not recognize the item type"sv);
}

// https://httpwg.org/specs/rfc9651.html#parse-key
static ErrorOr<String> parse_a_key(GenericLexer lexer)
{
    // 1. If the first character of input_string is not lcalpha or "*", fail parsing.
    auto first_character = lexer.peek();
    if ((first_character < 'a' || first_character > 'z') && first_character != '*')
        return Error::from_string_view("Invalid start of a key"sv);

    // 2. Let output_string be an empty string.
    // 3. While input_string is not empty:
    //     1. If the first character of input_string is not one of lcalpha, DIGIT, "_", "-", ".", or "*", return output_string.
    //     2. Let char be the result of consuming the first character of input_string.
    //     3. Append char to output_string.
    // 4. Return output_string.
    auto output_string = lexer.consume_until([](char c) { return (c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '_' && c != '-' && c != '.' && c != '*'; });
    return TRY(String::from_utf8(output_string));
}

// https://httpwg.org/specs/rfc9651.html#parse-param
static ErrorOr<Parameters> parse_parameters(GenericLexer lexer)
{
    // 1. Let parameters be an empty, ordered map.
    HashMap<String, StructuredFieldBareItem> parameters {};

    // 2. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. If the first character of input_string is not ";", exit the loop.
        if (lexer.peek() != ';')
            break;

        // 2. Consume the ";" character from the beginning of input_string.
        lexer.consume();

        // 3. Discard any leading SP characters from input_string.
        lexer.consume_while(is_sp_character);

        // 4. Let param_key be the result of running Parsing a Key (Section 4.2.3.3) with input_string.
        auto param_key = TRY(parse_a_key(lexer));

        // 5. Let param_value be Boolean true.
        StructuredFieldBareItem param_value = StructuredFieldBoolean { true };

        // 6. If the first character of input_string is "=":
        if (lexer.peek() == '=') {
            // 1. Consume the "=" character at the beginning of input_string.
            lexer.consume();

            // 2. Let param_value be the result of running Parsing a Bare Item (Section 4.2.3.1) with input_string.
            param_value = TRY(parse_a_bare_item(lexer));
        }

        // 7. If parameters already contains a key param_key (comparing character for character), overwrite its value with param_value.
        // 8. Otherwise, append key param_key with value param_value to parameters.
        parameters.set(param_key, param_value);
    }

    // 3. Return parameters.
    return parameters;
}

// https://httpwg.org/specs/rfc9651.html#parse-item
static ErrorOr<StructuredFieldItem> parse_an_item(GenericLexer lexer)
{
    // 1. Let bare_item be the result of running Parsing a Bare Item (Section 4.2.3.1) with input_string.
    auto bare_item = TRY(parse_a_bare_item(lexer));

    // 2. Let parameters be the result of running Parsing Parameters (Section 4.2.3.2) with input_string.
    auto parameters = TRY(parse_parameters(lexer));

    // 3. Return the tuple (bare_item, parameters).
    return StructuredFieldItem { .item = bare_item, .parameters = parameters };
}

// https://httpwg.org/specs/rfc9651.html#parse-innerlist
static ErrorOr<StructuredFieldInnerList> parse_an_inner_list(GenericLexer lexer)
{
    // 1. Consume the first character of input_string; if it is not "(", fail parsing.
    lexer.consume('(');

    // 2. Let inner_list be an empty array.
    Vector<StructuredFieldItem> inner_list;

    // 3. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Discard any leading SP characters from input_string.
        lexer.consume_while(is_sp_character);

        // 2. If the first character of input_string is ")":
        if (lexer.peek() == ')') {
            // 1. Consume the first character of input_string.
            lexer.consume();

            // 2. Let parameters be the result of running Parsing Parameters (Section 4.2.3.2) with input_string.
            auto parameters = TRY(parse_parameters(lexer));

            // 3. Return the tuple (inner_list, parameters).
            return StructuredFieldInnerList {
                .members = inner_list,
                .parameters = parameters
            };
        }

        // 3. Let item be the result of running Parsing an Item (Section 4.2.3) with input_string.
        auto item = TRY(parse_an_item(lexer));

        // 4. Append item to inner_list.
        inner_list.append(item);

        // 5. If the first character of input_string is not SP or ")", fail parsing.
        auto first_character = lexer.peek();
        if (!is_sp_character(first_character) && first_character != ')')
            return Error::from_string_view("Expected a closing bracket or space after an inner list entry"sv);
    }

    // 4. The end of the Inner List was not found; fail parsing.
    return Error::from_string_view("The end of the inner list was not found"sv);
}

// https://httpwg.org/specs/rfc9651.html#parse-item-or-list
static ErrorOr<StructuredFieldItemOrInnerList> parse_an_item_or_inner_list(GenericLexer lexer)
{
    // 1. If the first character of input_string is "(", return the result of running Parsing an Inner List (Section 4.2.1.2) with input_string.
    if (lexer.peek() == '(')
        return TRY(parse_an_inner_list(lexer));

    // 2. Return the result of running Parsing an Item (Section 4.2.3) with input_string.
    return TRY(parse_an_item(lexer));
}

// https://httpwg.org/specs/rfc9651.html#parse-list
static ErrorOr<StructuredFieldList> parse_a_list(GenericLexer lexer)
{
    // 1. Let dictionary be an empty, ordered map.
    Vector<StructuredFieldItemOrInnerList> members;

    // 2. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Append the result of running Parsing an Item or Inner List (Section 4.2.1.1) with input_string to members.
        members.append(TRY(parse_an_item_or_inner_list(lexer)));

        // 2. Discard any leading OWS characters from input_string.
        lexer.consume_while(is_ows_character);

        // 3. If input_string is empty, return dictionary.
        if (lexer.is_eof())
            return StructuredFieldList { members };

        // 4. Consume the first character of input_string; if it is not ",", fail parsing.
        if (lexer.consume() != ',')
            return Error::from_string_view("Expected a comma after a list entry"sv);

        // 5. Discard any leading OWS characters from input_string.
        lexer.consume_while(is_ows_character);

        // 6. If input_string is empty, there is a trailing comma; fail parsing.
        if (lexer.is_eof())
            return Error::from_string_view("There is a trailing comma"sv);
    }

    // 3. No structured data has been found; return members (which is empty).
    return StructuredFieldList { members };
}

// https://httpwg.org/specs/rfc9651.html#parse-dictionary
static ErrorOr<StructuredFieldDictionary> parse_a_dictionary(GenericLexer lexer)
{
    // 1. Let dictionary be an empty, ordered map.
    HashMap<String, StructuredFieldItemOrInnerList> dictionary;

    // 2. While input_string is not empty:
    while (!lexer.is_eof()) {
        // 1. Let this_key be the result of running Parsing a Key (Section 4.2.3.3) with input_string.
        auto key = TRY(parse_a_key(lexer));

        // 2. If the first character of input_string is "=":
        Optional<StructuredFieldItemOrInnerList> member;
        if (lexer.peek() == '=') {
            // 1. Consume the first character of input_string.
            lexer.consume();

            // 2. Let member be the result of running Parsing an Item or Inner List (Section 4.2.1.1) with input_string.
            member = TRY(parse_an_item_or_inner_list(lexer));
        }

        // 3. Otherwise:
        else {
            // 1. Let value be Boolean true.
            StructuredFieldBareItem value = StructuredFieldBoolean { true };

            // 2. Let parameters be the result of running Parsing Parameters (Section 4.2.3.2) with input_string.
            auto parameters = TRY(parse_parameters(lexer));

            // 3. Let member be the tuple (value, parameters).
            member = StructuredFieldItem { .item = value, .parameters = parameters };
        }

        // 4. If dictionary already contains a key this_key (comparing character for character), overwrite its value with member.
        // 5. Otherwise, append key this_key with value member to dictionary.
        dictionary.set(key, member.value());

        // 6. Discard any leading OWS characters from input_string.
        lexer.consume_while(is_ows_character);

        // 7. If input_string is empty, return dictionary.
        if (lexer.is_eof())
            return StructuredFieldDictionary { dictionary };

        // 8. Consume the first character of input_string; if it is not ",", fail parsing.
        if (lexer.consume() != ',')
            return Error::from_string_view("Expected a comma after a dictionary entry"sv);

        // 9. Discard any leading OWS characters from input_string.
        lexer.consume_while(is_ows_character);

        // 10. If input_string is empty, there is a trailing comma; fail parsing.
        if (lexer.is_eof())
            return Error::from_string_view("There is a trailing comma"sv);
    }
    // 3. No structured data has been found; return dictionary (which is empty).
    return StructuredFieldDictionary { dictionary };
}

// https://httpwg.org/specs/rfc9651.html#text-parse
ErrorOr<StructuredFieldValue> parse_structured_fields(StringView input_string, StructuredFieldType header_type)
{
    // 1. Convert input_bytes into an ASCII string input_string; if conversion fails, fail parsing.
    if (!input_string.is_ascii())
        return Error::from_string_view("Structured field is not an ASCII string"sv);

    // 2. Discard any leading SP characters from input_string.
    GenericLexer lexer { input_string };
    lexer.consume_while(is_sp_character);

    // 3. If field_type is "list", let output be the result of running Parsing a List (Section 4.2.1) with input_string.
    Optional<StructuredFieldValue> output;
    if (header_type == StructuredFieldType::List) {
        output = TRY(parse_a_list(lexer));
    }

    // 4. If field_type is "dictionary", let output be the result of running Parsing a Dictionary (Section 4.2.2) with input_string.
    else if (header_type == StructuredFieldType::Dictionary) {
        output = TRY(parse_a_dictionary(lexer));
    }

    // 5. If field_type is "item", let output be the result of running Parsing an Item (Section 4.2.3) with input_string.
    else if (header_type == StructuredFieldType::Item) {
        output = TRY(parse_an_item(lexer));
    }

    // 6. Discard any leading SP characters from input_string.
    lexer.consume_while(is_sp_character);

    // 7. If input_string is not empty, fail parsing.
    if (!lexer.is_eof())
        return Error::from_string_view("Structured field contains extra characters"sv);

    // 8. Otherwise, return output.
    return output.value();
}

}
