/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/GenericLexer.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/Utf16View.h>
#include <LibHTTP/HTTP.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/Method.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <LibURL/Parser.h>

namespace HTTP {

Header Header::isomorphic_encode(StringView name, StringView value)
{
    return { TextCodec::isomorphic_encode(name), TextCodec::isomorphic_encode(value) };
}

Header Header::isomorphic_encode(StringView name, Utf16View value)
{
    return { TextCodec::isomorphic_encode(name), TextCodec::isomorphic_encode(value) };
}

// https://www.rfc-editor.org/rfc/rfc9110.html#name-recipient-requirements
static Optional<Vector<ByteString>> extract_token_headers(ByteString const& value)
{
    Vector<ByteString> result;
    for (auto& part : value.split(',', SplitBehavior::Nothing)) {
        auto trimmed = part.trim(HTTP_WHITESPACE, TrimMode::Both);
        if (trimmed.is_empty())
            continue;
        if (!is_header_name(trimmed))
            return {};
        result.append(move(trimmed));
    }
    return result;
}

// https://fetch.spec.whatwg.org/#extract-header-values
Optional<Vector<ByteString>> Header::extract_header_values() const
{
    // NB: There is some specification work to try and rework this function, see: https://github.com/whatwg/fetch/issues/814

    // 1. If parsing header’s value, per the ABNF for header’s name, fails, then return failure.
    // 2. Return one or more values resulting from parsing header’s value, per the ABNF for header’s name.

    // ABNF taken from:
    //  * https://fetch.spec.whatwg.org/#http-new-header-syntax
    //  * https://httpwg.org/specs/rfc9110.html#field.accept-ranges

    // Access-Control-Expose-Headers = #field-name (field-name = token)
    // Access-Control-Allow-Headers  = #field-name (field-name = token)
    // Access-Control-Allow-Methods  = #method     (method = token)
    if (name.is_one_of_ignoring_ascii_case(
            "Access-Control-Expose-Headers"sv,
            "Access-Control-Allow-Headers"sv,
            "Access-Control-Allow-Methods"sv)) {
        return extract_token_headers(value);
    }

    // Access-Control-Request-Headers = 1#field-name      (field-name = token)
    // Accept-Ranges                  = acceptable-ranges (acceptable-ranges = 1#range-unit, range-unit = token)
    // Referrer-Policy                = 1#policy-token    (https://w3c.github.io/webappsec-referrer-policy/#referrer-policy-header)
    if (name.is_one_of_ignoring_ascii_case(
            "Access-Control-Request-Headers"sv,
            "Accept-Ranges"sv,
            "Referrer-Policy"sv)) {
        if (auto headers = extract_token_headers(value); headers.has_value()) {
            if (headers->is_empty())
                return {};
            return headers;
        }
        return {};
    }

    // FIXME: What other headers should we handle here (or elsewhere?)
    return Vector { value };
}

// https://fetch.spec.whatwg.org/#header-name
bool is_header_name(StringView header_name)
{
    // A header name is a byte sequence that matches the field-name token production.
    return is_token(header_name);
}

// https://fetch.spec.whatwg.org/#header-value
bool is_header_value(StringView header_value)
{
    // A header value is a byte sequence that matches the following conditions:
    // - Has no leading or trailing HTTP tab or space bytes.
    // - Contains no 0x00 (NUL) or HTTP newline bytes.
    if (header_value.is_empty())
        return true;

    auto first_byte = header_value[0];
    auto last_byte = header_value[header_value.length() - 1];

    if (is_http_tab_or_space(first_byte) || is_http_tab_or_space(last_byte))
        return false;

    return !any_of(header_value, [](auto byte) {
        return byte == 0x00 || is_http_newline(byte);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-value-normalize
StringView normalize_header_value(StringView potential_value)
{
    // To normalize a byte sequence potentialValue, remove any leading and trailing HTTP whitespace bytes from
    // potentialValue.
    if (potential_value.is_empty())
        return {};
    return potential_value.trim(HTTP_WHITESPACE, TrimMode::Both);
}

// https://fetch.spec.whatwg.org/#forbidden-header-name
bool is_forbidden_request_header(Header const& header)
{
    auto const& [name, value] = header;

    // 1. If name is a byte-case-insensitive match for one of:
    // [...]
    // then return true.
    if (name.is_one_of_ignoring_ascii_case(
            "Accept-Charset"sv,
            "Accept-Encoding"sv,
            "Access-Control-Request-Headers"sv,
            "Access-Control-Request-Method"sv,
            "Connection"sv,
            "Content-Length"sv,
            "Cookie"sv,
            "Cookie2"sv,
            "Date"sv,
            "DNT"sv,
            "Expect"sv,
            "Host"sv,
            "Keep-Alive"sv,
            "Origin"sv,
            "Referer"sv,
            "Set-Cookie"sv,
            "TE"sv,
            "Trailer"sv,
            "Transfer-Encoding"sv,
            "Upgrade"sv,
            "Via"sv)) {
        return true;
    }

    // 2. If name when byte-lowercased starts with `proxy-` or `sec-`, then return true.
    if (name.starts_with("proxy-"sv, CaseSensitivity::CaseInsensitive)
        || name.starts_with("sec-"sv, CaseSensitivity::CaseInsensitive)) {
        return true;
    }

    // 3. If name is a byte-case-insensitive match for one of:
    // - `X-HTTP-Method`
    // - `X-HTTP-Method-Override`
    // - `X-Method-Override`
    // then:
    if (name.is_one_of_ignoring_ascii_case(
            "X-HTTP-Method"sv,
            "X-HTTP-Method-Override"sv,
            "X-Method-Override"sv)) {
        // 1. Let parsedValues be the result of getting, decoding, and splitting value.
        auto parsed_values = get_decode_and_split_header_value(value);

        // 2. For each method of parsedValues: if the isomorphic encoding of method is a forbidden method, then return true.
        // NB: The values returned from get_decode_and_split_header_value have already been decoded.
        if (any_of(parsed_values, [](auto const& method) { return is_forbidden_method(method); }))
            return true;
    }

    // 4. Return false.
    return false;
}

// https://fetch.spec.whatwg.org/#forbidden-response-header-name
bool is_forbidden_response_header_name(StringView header_name)
{
    // A forbidden response-header name is a header name that is a byte-case-insensitive match for one of:
    // - `Set-Cookie`
    // - `Set-Cookie2`
    return header_name.is_one_of_ignoring_ascii_case(
        "Set-Cookie"sv,
        "Set-Cookie2"sv);
}

// https://fetch.spec.whatwg.org/#header-value-get-decode-and-split
Vector<String> get_decode_and_split_header_value(StringView value)
{
    // 1. Let input be the result of isomorphic decoding value.
    auto input = TextCodec::isomorphic_decode(value);

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    GenericLexer lexer { input };

    // 3. Let values be a list of strings, initially « ».
    Vector<String> values;

    // 4. Let temporaryValue be the empty string.
    StringBuilder temporary_value_builder;

    // 5. While true:
    while (true) {
        // 1. Append the result of collecting a sequence of code points that are not U+0022 (") or U+002C (,) from
        //    input, given position, to temporaryValue.
        // NOTE: The result might be the empty string.
        temporary_value_builder.append(lexer.consume_until(is_any_of("\","sv)));

        // 2. If position is not past the end of input and the code point at position within input is U+0022 ("):
        if (!lexer.is_eof() && lexer.peek() == '"') {
            // 1. Append the result of collecting an HTTP quoted string from input, given position, to temporaryValue.
            temporary_value_builder.append(collect_an_http_quoted_string(lexer));

            // 2. If position is not past the end of input, then continue.
            if (!lexer.is_eof())
                continue;
        }

        // 3. Remove all HTTP tab or space from the start and end of temporaryValue.
        auto temporary_value = MUST(String::from_utf8(temporary_value_builder.string_view().trim(HTTP_TAB_OR_SPACE, TrimMode::Both)));

        // 4. Append temporaryValue to values.
        values.append(move(temporary_value));

        // 5. Set temporaryValue to the empty string.
        temporary_value_builder.clear();

        // 6. If position is past the end of input, then return values.
        if (lexer.is_eof())
            return values;

        // 7. Assert: the code point at position within input is U+002C (,).
        VERIFY(lexer.peek() == ',');

        // 8. Advance position by 1.
        lexer.ignore(1);
    }
}

// https://fetch.spec.whatwg.org/#header-value-get-decode-and-split
// AD-HOC: This does not split at a comma within a link's <...> target, which a URL may contain.
Vector<String> get_decode_and_split_link_header_value(StringView value)
{
    // 1. Let input be the result of isomorphic decoding value.
    auto input = TextCodec::isomorphic_decode(value);

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    GenericLexer lexer { input };

    // 3. Let values be a list of strings, initially « ».
    Vector<String> values;

    // 4. Let temporaryValue be the empty string.
    StringBuilder temporary_value_builder;

    // 5. While true:
    while (true) {
        // 1. Append the result of collecting a sequence of code points that are not U+0022 (") or U+002C (,) from
        //    input, given position, to temporaryValue.
        // NOTE: The result might be the empty string.
        temporary_value_builder.append(lexer.consume_until(is_any_of("\",<"sv)));

        // 2. If position is not past the end of input and the code point at position within input is U+0022 ("):
        if (!lexer.is_eof() && lexer.peek() == '"') {
            // 1. Append the result of collecting an HTTP quoted string from input, given position, to temporaryValue.
            temporary_value_builder.append(collect_an_http_quoted_string(lexer));

            // 2. If position is not past the end of input, then continue.
            if (!lexer.is_eof())
                continue;
        }
        // AD-HOC: Step 1 also stops at U+003C (<). Append the code points from there up to and including the next
        //         U+003E (>), and continue unless that reached the end of input.
        else if (!lexer.is_eof() && lexer.peek() == '<') {
            temporary_value_builder.append(lexer.consume_until('>'));
            if (lexer.consume_specific('>'))
                temporary_value_builder.append('>');
            if (!lexer.is_eof())
                continue;
        }

        // 3. Remove all HTTP tab or space from the start and end of temporaryValue.
        auto temporary_value = MUST(String::from_utf8(temporary_value_builder.string_view().trim(HTTP_TAB_OR_SPACE, TrimMode::Both)));

        // 4. Append temporaryValue to values.
        values.append(move(temporary_value));

        // 5. Set temporaryValue to the empty string.
        temporary_value_builder.clear();

        // 6. If position is past the end of input, then return values.
        if (lexer.is_eof())
            return values;

        // 7. Assert: the code point at position within input is U+002C (,).
        VERIFY(lexer.peek() == ',');

        // 8. Advance position by 1.
        lexer.ignore(1);
    }
}

// https://fetch.spec.whatwg.org/#convert-header-names-to-a-sorted-lowercase-set
Vector<ByteString> convert_header_names_to_a_sorted_lowercase_set(ReadonlySpan<ByteString> header_names)
{
    // 1. Let headerNamesSet be a new ordered set.
    HashTable<StringView, CaseInsensitiveASCIIStringTraits> header_names_seen;
    Vector<ByteString> header_names_set;

    // 2. For each name of headerNames, append the result of byte-lowercasing name to headerNamesSet.
    for (auto const& name : header_names) {
        if (header_names_seen.contains(name))
            continue;

        header_names_seen.set(name);
        header_names_set.append(name.to_lowercase());
    }

    // 3. Return the result of sorting headerNamesSet in ascending order with byte less than.
    quick_sort(header_names_set);
    return header_names_set;
}

// https://fetch.spec.whatwg.org/#build-a-content-range
ByteString build_content_range(u64 range_start, u64 range_end, u64 full_length)
{
    // 1. Let contentRange be `bytes `.
    // 2. Append rangeStart, serialized and isomorphic encoded, to contentRange.
    // 3. Append 0x2D (-) to contentRange.
    // 4. Append rangeEnd, serialized and isomorphic encoded to contentRange.
    // 5. Append 0x2F (/) to contentRange.
    // 6. Append fullLength, serialized and isomorphic encoded to contentRange.
    // 7. Return contentRange.
    return ByteString::formatted("bytes {}-{}/{}", range_start, range_end, full_length);
}

// https://fetch.spec.whatwg.org/#simple-range-header-value
Optional<RangeHeaderValue> parse_single_range_header_value(StringView value, bool allow_whitespace)
{
    // 1. Let data be the isomorphic decoding of value.
    auto data = TextCodec::isomorphic_decode(value);

    // 2. If data does not start with "bytes", then return failure.
    if (!data.starts_with_bytes("bytes"sv))
        return {};

    // 3. Let position be a position variable for data, initially pointing at the 5th code point of data.
    GenericLexer lexer { data };
    lexer.ignore(5);

    // 4. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 5. If the code point at position within data is not U+003D (=), then return failure.
    // 6. Advance position by 1.
    if (!lexer.consume_specific('='))
        return {};

    // 7. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 8. Let rangeStart be the result of collecting a sequence of code points that are ASCII digits, from data given position.
    auto range_start = lexer.consume_while(is_ascii_digit);

    // 9. Let rangeStartValue be rangeStart, interpreted as decimal number, if rangeStart is not the empty string;
    //    otherwise null.
    auto range_start_value = range_start.to_number<u64>();

    // 10. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 11. If the code point at position within data is not U+002D (-), then return failure.
    // 12. Advance position by 1.
    if (!lexer.consume_specific('-'))
        return {};

    // 13. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 14. Let rangeEnd be the result of collecting a sequence of code points that are ASCII digits, from data given position.
    auto range_end = lexer.consume_while(is_ascii_digit);

    // 15. Let rangeEndValue be rangeEnd, interpreted as decimal number, if rangeEnd is not the empty string; otherwise null.
    auto range_end_value = range_end.to_number<u64>();

    // 16. If position is not past the end of data, then return failure.
    if (!lexer.is_eof())
        return {};

    // 17. If rangeEndValue and rangeStartValue are null, then return failure.
    if (!range_end_value.has_value() && !range_start_value.has_value())
        return {};

    // 18. If rangeStartValue and rangeEndValue are numbers, and rangeStartValue is greater than rangeEndValue, then
    //     return failure.
    if (range_start_value.has_value() && range_end_value.has_value() && *range_start_value > *range_end_value)
        return {};

    // 19. Return (rangeStartValue, rangeEndValue).
    return RangeHeaderValue { range_start_value, range_end_value };
}

Optional<String const&> Link::target_attribute(StringView name) const
{
    auto attribute = target_attributes->attributes.first_matching([&](auto const& attribute) { return attribute.name == name; });
    if (!attribute.has_value())
        return {};
    return attribute->value;
}

// https://www.rfc-editor.org/rfc/rfc8288#appendix-B.4
static String parse_a_quoted_string(GenericLexer& lexer)
{
    // 1. Let output be an empty string.
    StringBuilder output;

    // 2. If the first character of input is not DQUOTE, return output.
    // 3. Discard the first character.
    if (!lexer.consume_specific('"'))
        return {};

    // 4. While input has content:
    while (!lexer.is_eof()) {
        // 1. If the first character is a backslash ("\"):
        if (lexer.consume_specific('\\')) {
            // 1. Discard the first character.
            // 2. If there is no more input, return output.
            if (lexer.is_eof())
                break;

            // 3. Else, consume the first character and append it to output.
            output.append(lexer.consume());
        }
        // 2. Else, if the first character is DQUOTE, discard it and return output.
        else if (lexer.consume_specific('"')) {
            break;
        }
        // 3. Else, consume the first character and append it to output.
        else {
            output.append(lexer.consume());
        }
    }

    // 5. Return output.
    return output.to_string_without_validation();
}

// https://www.rfc-editor.org/rfc/rfc8288#appendix-B.3
static Vector<LinkParameter> parse_parameters(GenericLexer& lexer)
{
    // 1. Let parameters be an empty list.
    Vector<LinkParameter> parameters;

    // 2. While input has content:
    while (!lexer.is_eof()) {
        // 1. Consume any leading OWS.
        lexer.ignore_while(is_http_tab_or_space);

        // 2. If the first character is not ";", return parameters.
        // 3. Discard the leading ";" character.
        if (!lexer.consume_specific(';'))
            return parameters;

        // 4. Consume any leading OWS.
        lexer.ignore_while(is_http_tab_or_space);

        // 5. Consume up to but not including the first BWS, "=", ";", or "," character, or up to the end of input,
        //    and let the result be parameter_name.
        auto parameter_name = lexer.consume_until(is_any_of("\t =;,"sv));

        // 6. Consume any leading BWS.
        lexer.ignore_while(is_http_tab_or_space);

        String parameter_value;

        // 7. If the next character is "=":
        if (lexer.consume_specific('=')) {
            // 1. Discard the leading "=" character.
            // 2. Consume any leading BWS.
            lexer.ignore_while(is_http_tab_or_space);

            // 3. If the next character is DQUOTE, let parameter_value be the result of Parsing a Quoted String
            //    (Appendix B.4) from input (consuming zero or more characters of it).
            if (lexer.next_is('"')) {
                parameter_value = parse_a_quoted_string(lexer);
            }
            // 4. Else, consume the contents up to but not including the first ";" or "," character, or up to the end
            //    of input, and let the results be parameter_value.
            // AD-HOC: Drop the OWS before that character, which the ABNF does not allow in a token.
            else {
                auto value = lexer.consume_until(is_any_of(";,"sv)).trim(HTTP_TAB_OR_SPACE, TrimMode::Right);
                parameter_value = String::from_utf8_without_validation(value.bytes());
            }

            // 5. If the last character of parameter_name is an asterisk ("*"), decode parameter_value according to
            //    [RFC8187]. Continue processing input if an unrecoverable error is encountered.
            // NB: Step 16 of Parsing a Link Field Value removes these parameters.
        }
        // 8. Else:
        //    1. Let parameter_value be an empty string.

        // 9. Case-normalise parameter_name to lowercase.
        // 10. Append (parameter_name, parameter_value) to parameters.
        parameters.append({ String::from_utf8_without_validation(parameter_name.bytes()).to_ascii_lowercase(), move(parameter_value) });

        // 11. Consume any leading OWS.
        lexer.ignore_while(is_http_tab_or_space);

        // 12. If the next character is "," or the end of input, stop processing input and return parameters.
        if (lexer.is_eof() || lexer.next_is(','))
            return parameters;
    }

    return parameters;
}

// https://www.rfc-editor.org/rfc/rfc8288#appendix-B.2
Vector<Link> parse_link_field_value(StringView field_value, URL::URL const& representation_url)
{
    GenericLexer lexer { field_value };

    // 1. Let links be an empty list.
    Vector<Link> links;

    // 2. While field_value has content:
    while (!lexer.is_eof()) {
        // 1. Consume any leading OWS.
        lexer.ignore_while(is_http_tab_or_space);

        // 2. If the first character is not "<", return links.
        // 3. Discard the first character ("<").
        if (!lexer.consume_specific('<'))
            return links;

        // 4. Consume up to but not including the first ">" character or end of field_value and let the result be
        //    target_string.
        auto target_string = lexer.consume_until('>');

        // 5. If the next character is not ">", return links.
        // 6. Discard the leading ">" character.
        if (!lexer.consume_specific('>'))
            return links;

        // 7. Let link_parameters be the result of Parsing Parameters (Appendix B.3) from field_value (consuming zero or
        //    more characters of it).
        auto link_parameters = parse_parameters(lexer);

        // 8. Let target_uri be the result of relatively resolving (as per [RFC3986], Section 5.2) target_string. Note
        //    that any base URI carried in the payload body is NOT used.
        // NB: The base URI is the URL of the representation. A target that fails to parse has no target_uri.
        auto target_uri = URL::Parser::basic_parse(target_string, representation_url);

        // 9. Let relations_string be the second item of the first tuple of link_parameters whose first item matches
        //    the string "rel" or the empty string ("") if it is not present.
        auto relation = link_parameters.first_matching([](auto const& parameter) { return parameter.name == "rel"sv; });
        auto relations_string = relation.has_value() ? relation->value.bytes_as_string_view() : ""sv;

        // 10. Split relations_string on RWS (removing it in the process) into a list of string relation_types.
        auto relation_types = relations_string.split_view_if(is_http_tab_or_space);

        // 11. Let context_string be the second item of the first tuple of link_parameters whose first item matches the
        //     string "anchor". If it is not present, context_string is the URL of the representation carrying the
        //     Link header [RFC7231], Section 3.1.4.1, serialised as a URI. Where the URL is anonymous,
        //     context_string is null.
        // 12. Let context_uri be the result of relatively resolving (as per [RFC3986], Section 5.2) context_string,
        //     unless context_string is null, in which case context is null. Note that any base URI carried in the
        //     payload body is NOT used.
        auto anchor = link_parameters.first_matching([](auto const& parameter) { return parameter.name == "anchor"sv; });
        auto context_uri = anchor.has_value() ? URL::Parser::basic_parse(anchor->value, representation_url) : Optional<URL::URL> { representation_url };

        // 13. Let target_attributes be an empty list.
        Vector<LinkParameter> target_attributes;

        // 14. For each tuple (param_name, param_value) of link_parameters:
        for (auto& parameter : link_parameters) {
            // 1. If param_name matches "rel" or "anchor", skip this tuple.
            if (parameter.name.is_one_of("rel"sv, "anchor"sv))
                continue;

            // 2. If param_name matches "media", "title", "title*", or "type" and target_attributes already contains a
            //    tuple whose first element matches the value of param_name, skip this tuple.
            if (parameter.name.is_one_of("media"sv, "title"sv, "title*"sv, "type"sv)
                && target_attributes.contains([&](auto const& attribute) { return attribute.name == parameter.name; })) {
                continue;
            }

            // 3. Append (param_name, param_value) to target_attributes.
            target_attributes.append(parameter);
        }

        // NB: Steps 15 and 16 are as corrected by https://www.rfc-editor.org/errata/eid5878.
        // 15. Let star_param_names be the set of param_names in the (param_name, param_value) tuples of
        //     target_attributes where the last character of param_name is an asterisk ("*").
        // 16. For each star_param_name in star_param_names:
        //     1. Let base_param_name be star_param_name with the last character removed.
        //     2. If the implementation does not choose to support an internationalised form of a parameter named
        //        base_param_name for any reason (including, but not limited to, it being prohibited by the
        //        parameter's specification), remove all tuples from target_attributes whose first member is
        //        star_param_name, and skip to the next star_param_name.
        //     3. Remove all tuples from target_attributes whose first member is base_param_name.
        //     4. Change the first member of all tuples in target_attributes whose first member is star_param_name to
        //        base_param_name.
        // NB: No internationalised form of a parameter is supported.
        target_attributes.remove_all_matching([](auto const& attribute) { return attribute.name.ends_with('*'); });

        auto shared_target_attributes = make_ref_counted<LinkTargetAttributes>(move(target_attributes));
        HashTable<String> appended_relation_types;

        // 17. For each relation_type in relation_types:
        for (auto relation_type : relation_types) {
            // 1. Case-normalise relation_type to lowercase.
            auto lowercase_relation_type = String::from_utf8_without_validation(relation_type.bytes()).to_ascii_lowercase();

            // AD-HOC: Skip a relation type that repeats an earlier one, which would only repeat the same link, matching
            //         other engines.
            if (appended_relation_types.set(lowercase_relation_type) != HashSetResult::InsertedNewEntry)
                continue;

            // 2. Append a link object to links with the target target_uri, relation type of relation_type, context of
            //    context_uri, and target attributes target_attributes.
            links.append({
                .target_uri = target_uri,
                .relation_type = move(lowercase_relation_type),
                .context_uri = context_uri,
                .target_attributes = shared_target_attributes,
            });
        }
    }

    // 3. Return links.
    return links;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, HTTP::Header const& header)
{
    TRY(encoder.encode(header.name));
    TRY(encoder.encode(header.value));
    return {};
}

template<>
ErrorOr<HTTP::Header> decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<ByteString>());
    auto value = TRY(decoder.decode<ByteString>());
    return HTTP::Header { move(name), move(value) };
}

}
