/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibHTTP/StructuredFields.h>

namespace HTTP::StructuredFields {

static ErrorOr<BareItem> parse_a_bare_item(GenericLexer&);

static bool is_sp(char character)
{
    return character == ' ';
}

static bool is_ows(char character)
{
    return character == ' ' || character == '\t';
}

static bool is_tchar(char character)
{
    return is_ascii_alphanumeric(character) || "!#$%&'*+-.^_`|~"sv.contains(character);
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-key
static ErrorOr<ByteString> parse_a_key(GenericLexer& input_string)
{
    // 1. If the first character of input_string is not lcalpha or "*", fail parsing.
    if (!is_ascii_lower_alpha(input_string.peek()) && !input_string.next_is('*'))
        return Error::from_string_literal("Key does not start with a lowercase letter or '*'");

    // 2. Let output_string be an empty string.
    // 3. While input_string is not empty:
    //    1. If the first character of input_string is not one of lcalpha, DIGIT, "_", "-", ".", or "*", return
    //       output_string.
    //    2. Let char be the result of consuming the first character of input_string.
    //    3. Append char to output_string.
    // 4. Return output_string.
    return ByteString { input_string.consume_while([](char character) {
        return is_ascii_lower_alpha(character) || is_ascii_digit(character) || "_-.*"sv.contains(character);
    }) };
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-parameters
static ErrorOr<Parameters> parse_parameters(GenericLexer& input_string)
{
    // 1. Let parameters be an empty, ordered map.
    Parameters parameters;

    // 2. While input_string is not empty:
    while (!input_string.is_eof()) {
        // 1. If the first character of input_string is not ";", exit the loop.
        // 2. Consume the ";" character from the beginning of input_string.
        if (!input_string.consume_specific(';'))
            break;

        // 3. Discard any leading SP characters from input_string.
        input_string.ignore_while(is_sp);

        // 4. Let param_key be the result of running Parsing a Key (Section 4.2.3.3) with input_string.
        auto param_key = TRY(parse_a_key(input_string));

        // 5. Let param_value be Boolean true.
        BareItem param_value = true;

        // 6. If the first character of input_string is "=":
        //    1. Consume the "=" character at the beginning of input_string.
        if (input_string.consume_specific('=')) {
            // 2. Let param_value be the result of running Parsing a Bare Item (Section 4.2.3.1) with input_string.
            param_value = TRY(parse_a_bare_item(input_string));
        }

        // 7. If parameters already contains a key param_key (comparing character for character), overwrite its value
        //    with param_value.
        // 8. Otherwise, append key param_key with value param_value to parameters.
        parameters.set(move(param_key), move(param_value));
    }

    // 3. Return parameters.
    return parameters;
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-an-integer-or-decim
static ErrorOr<BareItem> parse_an_integer_or_decimal(GenericLexer& input_string)
{
    enum class Type : u8 {
        Integer,
        Decimal,
    };

    // 1. Let type be "integer".
    auto type = Type::Integer;

    // 2. Let sign be 1.
    i64 sign = 1;

    // 3. Let input_number be an empty string.
    StringBuilder input_number;

    // 4. If the first character of input_string is "-", consume it and set sign to -1.
    if (input_string.consume_specific('-'))
        sign = -1;

    // 5. If input_string is empty, there is an empty integer; fail parsing.
    if (input_string.is_eof())
        return Error::from_string_literal("Empty integer");

    // 6. If the first character of input_string is not a DIGIT, fail parsing.
    if (!is_ascii_digit(input_string.peek()))
        return Error::from_string_literal("Number does not start with a digit");

    // 7. While input_string is not empty:
    while (!input_string.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto character = input_string.consume();

        // 2. If char is a DIGIT, append it to input_number.
        if (is_ascii_digit(character)) {
            input_number.append(character);
        }
        // 3. Else, if type is "integer" and char is ".":
        else if (type == Type::Integer && character == '.') {
            // 1. If input_number contains more than 12 characters, fail parsing.
            if (input_number.length() > 12)
                return Error::from_string_literal("Decimal has too many integer digits");

            // 2. Otherwise, append char to input_number and set type to "decimal".
            input_number.append(character);
            type = Type::Decimal;
        }
        // 4. Otherwise, prepend char to input_string, and exit the loop.
        else {
            input_string.retreat();
            break;
        }

        // 5. If type is "integer" and input_number contains more than 15 characters, fail parsing.
        if (type == Type::Integer && input_number.length() > 15)
            return Error::from_string_literal("Integer has too many digits");

        // 6. If type is "decimal" and input_number contains more than 16 characters, fail parsing.
        if (type == Type::Decimal && input_number.length() > 16)
            return Error::from_string_literal("Decimal has too many digits");
    }

    auto input_number_view = input_number.string_view();
    Variant<i64, double> output_number = i64 { 0 };

    // 8. If type is "integer":
    if (type == Type::Integer) {
        // 1. Let output_number be an Integer that is the result of parsing input_number as an integer.
        output_number = input_number_view.to_number<i64>().value();
    }
    // 9. Otherwise:
    else {
        // 1. If the final character of input_number is ".", fail parsing.
        if (input_number_view.ends_with('.'))
            return Error::from_string_literal("Decimal has no fractional digits");

        // 2. If the number of characters after "." in input_number is greater than three, fail parsing.
        if (input_number_view.length() - input_number_view.find('.').value() - 1 > 3)
            return Error::from_string_literal("Decimal has too many fractional digits");

        // 3. Let output_number be a Decimal that is the result of parsing input_number as a decimal number.
        output_number = input_number_view.to_number<double>().value();
    }

    // 10. Let output_number be the product of output_number and sign.
    // 11. Return output_number.
    return output_number.visit([&](auto number) -> BareItem { return number * sign; });
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-string
static ErrorOr<ByteString> parse_a_string(GenericLexer& input_string)
{
    // 1. Let output_string be an empty string.
    StringBuilder output_string;

    // 2. If the first character of input_string is not DQUOTE, fail parsing.
    // 3. Discard the first character of input_string.
    if (!input_string.consume_specific('"'))
        return Error::from_string_literal("String does not start with a double quote");

    // 4. While input_string is not empty:
    while (!input_string.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto character = input_string.consume();

        // 2. If char is a backslash ("\"):
        if (character == '\\') {
            // 1. If input_string is now empty, fail parsing.
            if (input_string.is_eof())
                return Error::from_string_literal("String ends in an escape");

            // 2. Let next_char be the result of consuming the first character of input_string.
            auto next_character = input_string.consume();

            // 3. If next_char is not DQUOTE or "\", fail parsing.
            if (next_character != '"' && next_character != '\\')
                return Error::from_string_literal("String has an invalid escape");

            // 4. Append next_char to output_string.
            output_string.append(next_character);
        }
        // 3. Else, if char is DQUOTE, return output_string.
        else if (character == '"') {
            return output_string.to_byte_string();
        }
        // 4. Else, if char is in the range %x00-1f or %x7f-ff (i.e., it is not in VCHAR or SP), fail parsing.
        else if (!is_ascii_printable(character)) {
            return Error::from_string_literal("String has a non-printable character");
        }
        // 5. Else, append char to output_string.
        else {
            output_string.append(character);
        }
    }

    // 5. Reached the end of input_string without finding a closing DQUOTE; fail parsing.
    return Error::from_string_literal("String has no closing double quote");
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-token
static ErrorOr<Token> parse_a_token(GenericLexer& input_string)
{
    // 1. If the first character of input_string is not ALPHA or "*", fail parsing.
    if (!is_ascii_alpha(input_string.peek()) && !input_string.next_is('*'))
        return Error::from_string_literal("Token does not start with a letter or '*'");

    // 2. Let output_string be an empty string.
    // 3. While input_string is not empty:
    //    1. If the first character of input_string is not in tchar, ":", or "/", return output_string.
    //    2. Let char be the result of consuming the first character of input_string.
    //    3. Append char to output_string.
    // 4. Return output_string.
    return Token { input_string.consume_while([](char character) {
        return is_tchar(character) || character == ':' || character == '/';
    }) };
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-byte-sequence
static ErrorOr<ByteBuffer> parse_a_byte_sequence(GenericLexer& input_string)
{
    // 1. If the first character of input_string is not ":", fail parsing.
    // 2. Discard the first character of input_string.
    if (!input_string.consume_specific(':'))
        return Error::from_string_literal("Byte sequence does not start with ':'");

    // 3. If there is not a ":" character before the end of input_string, fail parsing.
    if (!input_string.remaining().contains(':'))
        return Error::from_string_literal("Byte sequence has no closing ':'");

    // 4. Let b64_content be the result of consuming content of input_string up to but not including the first
    //    instance of the character ":".
    auto b64_content = input_string.consume_until(':');

    // 5. Consume the ":" character at the beginning of input_string.
    input_string.consume();

    // 6. If b64_content contains a character not included in ALPHA, DIGIT, "+", "/", and "=", fail parsing.
    auto is_base64_character = [](char character) {
        return is_ascii_alphanumeric(character) || character == '+' || character == '/' || character == '=';
    };
    if (!all_of(b64_content, is_base64_character))
        return Error::from_string_literal("Byte sequence has a non-base64 character");

    // 7. Let binary_content be the result of base64-decoding [RFC4648] b64_content, synthesizing padding if necessary
    //    (note the requirements about recipient behavior below). If base64 decoding fails, parsing fails.
    // NB: Recipients should neither require "=" padding nor reject non-zero pad bits, which the loose last chunk
    //     handling allows.
    auto binary_content = TRY(decode_base64(b64_content, AK::LastChunkHandling::Loose));

    // 8. Return binary_content.
    return binary_content;
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-boolean
static ErrorOr<bool> parse_a_boolean(GenericLexer& input_string)
{
    // 1. If the first character of input_string is not "?", fail parsing.
    // 2. Discard the first character of input_string.
    if (!input_string.consume_specific('?'))
        return Error::from_string_literal("Boolean does not start with '?'");

    // 3. If the first character of input_string matches "1", discard the first character, and return true.
    if (input_string.consume_specific('1'))
        return true;

    // 4. If the first character of input_string matches "0", discard the first character, and return false.
    if (input_string.consume_specific('0'))
        return false;

    // 5. No value has matched; fail parsing.
    return Error::from_string_literal("Boolean is neither '?0' nor '?1'");
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-date
static ErrorOr<Date> parse_a_date(GenericLexer& input_string)
{
    // 1. If the first character of input_string is not "@", fail parsing.
    // 2. Discard the first character of input_string.
    if (!input_string.consume_specific('@'))
        return Error::from_string_literal("Date does not start with '@'");

    // 3. Let output_date be the result of running Parsing an Integer or Decimal (Section 4.2.4) with input_string.
    auto output_date = TRY(parse_an_integer_or_decimal(input_string));

    // 4. If output_date is a Decimal, fail parsing.
    if (output_date.has<double>())
        return Error::from_string_literal("Date is a decimal");

    // 5. Return output_date.
    return Date { output_date.get<i64>() };
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-display-string
static ErrorOr<DisplayString> parse_a_display_string(GenericLexer& input_string)
{
    // 1. If the first two characters of input_string are not "%" followed by DQUOTE, fail parsing.
    // 2. Discard the first two characters of input_string.
    if (!input_string.consume_specific("%\""sv))
        return Error::from_string_literal("Display string does not start with '%\"'");

    // 3. Let byte_array be an empty byte array.
    StringBuilder byte_array;

    // 4. While input_string is not empty:
    while (!input_string.is_eof()) {
        // 1. Let char be the result of consuming the first character of input_string.
        auto character = input_string.consume();

        // 2. If char is in the range %x00-1f or %x7f-ff (i.e., it is not in VCHAR or SP), fail parsing.
        if (!is_ascii_printable(character))
            return Error::from_string_literal("Display string has a non-printable character");

        // 3. If char is "%":
        if (character == '%') {
            // 1. Let octet_hex be the result of consuming two characters from input_string. If there are not two
            //    characters, fail parsing.
            if (input_string.tell_remaining() < 2)
                return Error::from_string_literal("Display string has a truncated percent-encoding");
            auto octet_hex = input_string.consume(2);

            // 2. If octet_hex contains characters outside the range %x30-39 or %x61-66 (i.e., it is not in 0-9 or
            //    lowercase a-f), fail parsing.
            auto is_lowercase_hex_digit = [](char character) {
                return is_ascii_digit(character) || (character >= 'a' && character <= 'f');
            };
            if (!all_of(octet_hex, is_lowercase_hex_digit))
                return Error::from_string_literal("Display string has an invalid percent-encoding");

            // 3. Let octet be the result of hex decoding octet_hex (Section 8 of [RFC4648]).
            auto octet = static_cast<char>((parse_ascii_hex_digit(octet_hex[0]) << 4) | parse_ascii_hex_digit(octet_hex[1]));

            // 4. Append octet to byte_array.
            byte_array.append(octet);
        }
        // 4. If char is DQUOTE:
        else if (character == '"') {
            // 1. Let unicode_sequence be the result of decoding byte_array as a UTF-8 string (Section 3 of [UTF8]).
            //    Fail parsing if decoding fails.
            if (!Utf8View { byte_array.string_view() }.validate(AllowLonelySurrogates::No))
                return Error::from_string_literal("Display string is not valid UTF-8");
            auto unicode_sequence = String::from_utf8_without_validation(byte_array.string_view().bytes());

            // 2. Return unicode_sequence.
            return DisplayString { move(unicode_sequence) };
        }
        // 5. Otherwise, if char is not "%" or DQUOTE:
        else {
            // 1. Let byte be the result of applying ASCII encoding to char.
            // 2. Append byte to byte_array.
            byte_array.append(character);
        }
    }

    // 5. Reached the end of input_string without finding a closing DQUOTE; fail parsing.
    return Error::from_string_literal("Display string has no closing double quote");
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-bare-item
static ErrorOr<BareItem> parse_a_bare_item(GenericLexer& input_string)
{
    auto first_character = input_string.peek();

    // 1. If the first character of input_string is a "-" or a DIGIT, return the result of running Parsing an Integer
    //    or Decimal (Section 4.2.4) with input_string.
    if (first_character == '-' || is_ascii_digit(first_character))
        return parse_an_integer_or_decimal(input_string);

    // 2. If the first character of input_string is a DQUOTE, return the result of running Parsing a String
    //    (Section 4.2.5) with input_string.
    if (first_character == '"')
        return BareItem { TRY(parse_a_string(input_string)) };

    // 3. If the first character of input_string is an ALPHA or "*", return the result of running Parsing a Token
    //    (Section 4.2.6) with input_string.
    if (is_ascii_alpha(first_character) || first_character == '*')
        return BareItem { TRY(parse_a_token(input_string)) };

    // 4. If the first character of input_string is ":", return the result of running Parsing a Byte Sequence
    //    (Section 4.2.7) with input_string.
    if (first_character == ':')
        return BareItem { TRY(parse_a_byte_sequence(input_string)) };

    // 5. If the first character of input_string is "?", return the result of running Parsing a Boolean
    //    (Section 4.2.8) with input_string.
    if (first_character == '?')
        return BareItem { TRY(parse_a_boolean(input_string)) };

    // 6. If the first character of input_string is "@", return the result of running Parsing a Date (Section 4.2.9)
    //    with input_string.
    if (first_character == '@')
        return BareItem { TRY(parse_a_date(input_string)) };

    // 7. If the first character of input_string is "%", return the result of running Parsing a Display String
    //    (Section 4.2.10) with input_string.
    if (first_character == '%')
        return BareItem { TRY(parse_a_display_string(input_string)) };

    // 8. Otherwise, the item type is unrecognized; fail parsing.
    return Error::from_string_literal("Unrecognized item type");
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-an-item
static ErrorOr<Item> parse_an_item(GenericLexer& input_string)
{
    // 1. Let bare_item be the result of running Parsing a Bare Item (Section 4.2.3.1) with input_string.
    auto bare_item = TRY(parse_a_bare_item(input_string));

    // 2. Let parameters be the result of running Parsing Parameters (Section 4.2.3.2) with input_string.
    auto parameters = TRY(parse_parameters(input_string));

    // 3. Return the tuple (bare_item, parameters).
    return Item { move(bare_item), move(parameters) };
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-an-inner-list
static ErrorOr<InnerList> parse_an_inner_list(GenericLexer& input_string)
{
    // 1. Consume the first character of input_string; if it is not "(", fail parsing.
    if (!input_string.consume_specific('('))
        return Error::from_string_literal("Inner list does not start with '('");

    // 2. Let inner_list be an empty array.
    Vector<Item> inner_list;

    // 3. While input_string is not empty:
    while (!input_string.is_eof()) {
        // 1. Discard any leading SP characters from input_string.
        input_string.ignore_while(is_sp);

        // 2. If the first character of input_string is ")":
        //    1. Consume the first character of input_string.
        if (input_string.consume_specific(')')) {
            // 2. Let parameters be the result of running Parsing Parameters (Section 4.2.3.2) with input_string.
            auto parameters = TRY(parse_parameters(input_string));

            // 3. Return the tuple (inner_list, parameters).
            return InnerList { move(inner_list), move(parameters) };
        }

        // 3. Let item be the result of running Parsing an Item (Section 4.2.3) with input_string.
        auto item = TRY(parse_an_item(input_string));

        // 4. Append item to inner_list.
        inner_list.append(move(item));

        // 5. If the first character of input_string is not SP or ")", fail parsing.
        if (!input_string.next_is(' ') && !input_string.next_is(')'))
            return Error::from_string_literal("Inner list item is not followed by ' ' or ')'");
    }

    // 4. The end of the Inner List was not found; fail parsing.
    return Error::from_string_literal("Inner list has no closing ')'");
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-an-item-or-inner-li
static ErrorOr<ItemOrInnerList> parse_an_item_or_inner_list(GenericLexer& input_string)
{
    // 1. If the first character of input_string is "(", return the result of running Parsing an Inner List
    //    (Section 4.2.1.2) with input_string.
    if (input_string.next_is('('))
        return ItemOrInnerList { TRY(parse_an_inner_list(input_string)) };

    // 2. Return the result of running Parsing an Item (Section 4.2.3) with input_string.
    return ItemOrInnerList { TRY(parse_an_item(input_string)) };
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-list
static ErrorOr<List> parse_a_list(GenericLexer& input_string)
{
    // 1. Let members be an empty array.
    List members;

    // 2. While input_string is not empty:
    while (!input_string.is_eof()) {
        // 1. Append the result of running Parsing an Item or Inner List (Section 4.2.1.1) with input_string to
        //    members.
        members.append(TRY(parse_an_item_or_inner_list(input_string)));

        // 2. Discard any leading OWS characters from input_string.
        input_string.ignore_while(is_ows);

        // 3. If input_string is empty, return members.
        if (input_string.is_eof())
            return members;

        // 4. Consume the first character of input_string; if it is not ",", fail parsing.
        if (!input_string.consume_specific(','))
            return Error::from_string_literal("List member is not followed by ','");

        // 5. Discard any leading OWS characters from input_string.
        input_string.ignore_while(is_ows);

        // 6. If input_string is empty, there is a trailing comma; fail parsing.
        if (input_string.is_eof())
            return Error::from_string_literal("List has a trailing comma");
    }

    // 3. No structured data has been found; return members (which is empty).
    return members;
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-a-dictionary
static ErrorOr<Dictionary> parse_a_dictionary(GenericLexer& input_string)
{
    // 1. Let dictionary be an empty, ordered map.
    Dictionary dictionary;

    // 2. While input_string is not empty:
    while (!input_string.is_eof()) {
        // 1. Let this_key be the result of running Parsing a Key (Section 4.2.3.3) with input_string.
        auto this_key = TRY(parse_a_key(input_string));

        Optional<ItemOrInnerList> member;

        // 2. If the first character of input_string is "=":
        //    1. Consume the first character of input_string.
        if (input_string.consume_specific('=')) {
            // 2. Let member be the result of running Parsing an Item or Inner List (Section 4.2.1.1) with
            //    input_string.
            member = TRY(parse_an_item_or_inner_list(input_string));
        }
        // 3. Otherwise:
        else {
            // 1. Let value be Boolean true.
            BareItem value = true;

            // 2. Let parameters be the result of running Parsing Parameters (Section 4.2.3.2) with input_string.
            auto parameters = TRY(parse_parameters(input_string));

            // 3. Let member be the tuple (value, parameters).
            member = Item { move(value), move(parameters) };
        }

        // 4. If dictionary already contains a key this_key (comparing character for character), overwrite its value
        //    with member.
        // 5. Otherwise, append key this_key with value member to dictionary.
        dictionary.set(move(this_key), member.release_value());

        // 6. Discard any leading OWS characters from input_string.
        input_string.ignore_while(is_ows);

        // 7. If input_string is empty, return dictionary.
        if (input_string.is_eof())
            return dictionary;

        // 8. Consume the first character of input_string; if it is not ",", fail parsing.
        if (!input_string.consume_specific(','))
            return Error::from_string_literal("Dictionary member is not followed by ','");

        // 9. Discard any leading OWS characters from input_string.
        input_string.ignore_while(is_ows);

        // 10. If input_string is empty, there is a trailing comma; fail parsing.
        if (input_string.is_eof())
            return Error::from_string_literal("Dictionary has a trailing comma");
    }

    // 3. No structured data has been found; return dictionary (which is empty).
    return dictionary;
}

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parsing-structured-fields
template<FieldType T>
ErrorOr<T> parse(StringView input_bytes)
{
    // 1. Convert input_bytes into an ASCII string input_string; if conversion fails, fail parsing.
    if (!all_of(input_bytes, [](char character) { return is_ascii(character); }))
        return Error::from_string_literal("Structured field is not ASCII");
    GenericLexer input_string { input_bytes };

    // 2. Discard any leading SP characters from input_string.
    input_string.ignore_while(is_sp);

    // 3. If field_type is "list", let output be the result of running Parsing a List (Section 4.2.1) with
    //    input_string.
    // 4. If field_type is "dictionary", let output be the result of running Parsing a Dictionary (Section 4.2.2) with
    //    input_string.
    // 5. If field_type is "item", let output be the result of running Parsing an Item (Section 4.2.3) with
    //    input_string.
    auto output = TRY([&] {
        if constexpr (IsSame<T, List>)
            return parse_a_list(input_string);
        else if constexpr (IsSame<T, Dictionary>)
            return parse_a_dictionary(input_string);
        else
            return parse_an_item(input_string);
    }());

    // 6. Discard any leading SP characters from input_string.
    input_string.ignore_while(is_sp);

    // 7. If input_string is not empty, fail parsing.
    if (!input_string.is_eof())
        return Error::from_string_literal("Structured field has trailing characters");

    // 8. Otherwise, return output.
    return output;
}

template ErrorOr<List> parse(StringView);
template ErrorOr<Dictionary> parse(StringView);
template ErrorOr<Item> parse(StringView);

}
