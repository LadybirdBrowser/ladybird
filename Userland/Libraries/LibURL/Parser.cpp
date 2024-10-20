/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/IntegralMath.h>
#include <AK/Optional.h>
#include <AK/SourceLocation.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <LibURL/Parser.h>
#include <LibUnicode/IDNA.h>

namespace URL {

// NOTE: This is similar to the LibC macro EOF = -1.
constexpr u32 end_of_file = 0xFFFFFFFF;

// https://url.spec.whatwg.org/#forbidden-host-code-point
static bool is_forbidden_host_code_point(u32 code_point)
{
    // A forbidden host code point is U+0000 NULL, U+0009 TAB, U+000A LF, U+000D CR, U+0020 SPACE,
    // U+0023 (#), U+002F (/), U+003A (:), U+003C (<), U+003E (>), U+003F (?), U+0040 (@), U+005B ([),
    // U+005C (\), U+005D (]), U+005E (^), or U+007C (|).
    return "\0\t\n\r #/:<>?@[\\]^|"sv.contains(code_point);
}

// https://url.spec.whatwg.org/#forbidden-domain-code-point
static bool is_forbidden_domain_code_point(u32 code_point)
{
    // A forbidden domain code point is a forbidden host code point, a C0 control, U+0025 (%), or U+007F DELETE.
    return is_forbidden_host_code_point(code_point) || is_ascii_c0_control(code_point) || code_point == '%' || code_point == 0x7F;
}

// https://url.spec.whatwg.org/#url-code-points
static bool is_url_code_point(u32 code_point)
{
    // The URL code points are ASCII alphanumeric, U+0021 (!), U+0024 ($), U+0026 (&),
    // U+0027 ('), U+0028 LEFT PARENTHESIS, U+0029 RIGHT PARENTHESIS, U+002A (*),
    // U+002B (+), U+002C (,), U+002D (-), U+002E (.), U+002F (/), U+003A (:),
    // U+003B (;), U+003D (=), U+003F (?), U+0040 (@), U+005F (_), U+007E (~), and code
    // points in the range U+00A0 to U+10FFFD, inclusive, excluding surrogates and
    // noncharacters.
    return is_ascii_alphanumeric(code_point) || "!$&'()*+,-./:;=?@_~"sv.contains(code_point)
        || (code_point >= 0x00A0 && code_point <= 0x10FFFD && !is_unicode_surrogate(code_point) && !is_unicode_noncharacter(code_point));
}

static void report_validation_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(URL_PARSER_DEBUG, "URL::Parser::basic_parse: Validation error! {}", location);
}

// https://url.spec.whatwg.org/#concept-opaque-host-parser
static Optional<Host> parse_opaque_host(StringView input)
{
    // 1. If input contains a forbidden host code point, host-invalid-code-point validation error, return failure.
    for (auto code_point : Utf8View { input }) {
        if (is_forbidden_host_code_point(code_point)) {
            report_validation_error();
            return {};
        }
    }

    // 2. If input contains a code point that is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
    // 3. If input contains a U+0025 (%) and the two code points following it are not ASCII hex digits, invalid-URL-unit validation error.
    // NOTE: These steps are not implemented because they are not cheap checks and exist just to report validation errors. With how we
    //       currently report validation errors, they are only useful for debugging efforts in the URL parsing code.

    // 4. Return the result of running UTF-8 percent-encode on input using the C0 control percent-encode set.
    return percent_encode(input, PercentEncodeSet::C0Control);
}

struct ParsedIPv4Number {
    u32 number { 0 };
    bool validation_error { false };
};

// https://url.spec.whatwg.org/#ipv4-number-parser
static Optional<ParsedIPv4Number> parse_ipv4_number(StringView input)
{
    // 1. If input is the empty string, then return failure.
    if (input.is_empty())
        return {};

    // 2. Let validationError be false.
    bool validation_error = false;

    // 3. Let R be 10.
    u8 radix = 10;

    // 4. If input contains at least two code points and the first two code points are either "0X" or "0x", then:
    if (input.length() >= 2 && (input.starts_with("0X"sv) || input.starts_with("0x"sv))) {
        // 1. Set validationError to true.
        validation_error = true;

        // 2. Remove the first two code points from input.
        input = input.substring_view(2);

        // 3. Set R to 16.
        radix = 16;
    }
    // 5. Otherwise, if input contains at least two code points and the first code point is U+0030 (0), then:
    else if (input.length() >= 2 && input[0] == '0') {
        // 1. Set validationError to true.
        validation_error = true;

        // 2. Remove the first code point from input.
        input = input.substring_view(1);

        // 3. Set R to 8.
        radix = 8;
    }

    // 6. If input is the empty string, then return (0, true).
    if (input.is_empty())
        return ParsedIPv4Number { 0, true };

    // 7. If input contains a code point that is not a radix-R digit, then return failure.
    if (radix == 8) {
        if (!all_of(input, [](auto character) { return is_ascii_octal_digit(character); }))
            return {};
    } else if (radix == 10) {
        if (!all_of(input, [](auto character) { return is_ascii_digit(character); }))
            return {};
    } else if (radix == 16) {
        if (!all_of(input, [](auto character) { return is_ascii_hex_digit(character); }))
            return {};
    } else {
        VERIFY_NOT_REACHED();
    }

    // 8. Let output be the mathematical integer value that is represented by input in radix-R notation, using ASCII hex digits for digits with values 0 through 15.
    Optional<u32> maybe_output;
    if (radix == 8)
        maybe_output = AK::StringUtils::convert_to_uint_from_octal(input);
    else if (radix == 10)
        maybe_output = input.to_number<u32>();
    else if (radix == 16)
        maybe_output = AK::StringUtils::convert_to_uint_from_hex(input);
    else
        VERIFY_NOT_REACHED();

    // NOTE: Parsing may have failed due to overflow.
    if (!maybe_output.has_value())
        return {};

    // 9. Return (output, validationError).
    return ParsedIPv4Number { maybe_output.value(), validation_error };
}

// https://url.spec.whatwg.org/#concept-ipv4-parser
static Optional<IPv4Address> parse_ipv4_address(StringView input)
{
    // 1. Let parts be the result of strictly splitting input on U+002E (.).
    auto parts = input.split_view("."sv, SplitBehavior::KeepEmpty);

    // 2. If the last item in parts is the empty string, then:
    if (parts.last().is_empty()) {
        // 1. IPv4-empty-part validation error.
        report_validation_error();

        // 2. If parts’s size is greater than 1, then remove the last item from parts.
        if (parts.size() > 1)
            parts.take_last();
    }

    // 3. If parts’s size is greater than 4, IPv4-too-many-parts validation error, return failure.
    if (parts.size() > 4) {
        report_validation_error();
        return {};
    }

    // 4. Let numbers be an empty list.
    Vector<u32, 4> numbers;

    // 5. For each part of parts:
    for (auto const& part : parts) {
        // 1. Let result be the result of parsing part.
        auto const result = parse_ipv4_number(part);

        // 2. If result is failure, IPv4-non-numeric-part validation error, return failure.
        if (!result.has_value()) {
            report_validation_error();
            return {};
        }

        // 3. If result[1] is true, IPv4-non-decimal-part validation error.
        if (result->validation_error)
            report_validation_error();

        // 4. Append result[0] to numbers.
        numbers.append(result->number);
    }

    // 6. If any item in numbers is greater than 255, IPv4-out-of-range-part validation error.
    // 7. If any but the last item in numbers is greater than 255, then return failure.
    for (size_t i = 0; i < numbers.size(); ++i) {
        if (numbers[i] > 255) {
            report_validation_error();
            if (i != numbers.size() - 1)
                return {};
        }
    }

    // 8. If the last item in numbers is greater than or equal to 256^(5 − numbers’s size), then return failure.
    if (numbers.last() >= AK::pow<size_t>(256, 5 - numbers.size()))
        return {};

    // 9. Let ipv4 be the last item in numbers.
    auto ipv4 = numbers.last();

    // 10. Remove the last item from numbers.
    numbers.take_last();

    // 11. Let counter be 0.
    u8 counter = 0;

    // 12. For each n of numbers:
    for (u32 n : numbers) {
        // 1. Increment ipv4 by n × 256^(3 − counter).
        ipv4 += n * AK::pow<size_t>(256, 3 - counter);

        // 2. Increment counter by 1.
        ++counter;
    }

    // 13. Return ipv4.
    return ipv4;
}

// https://url.spec.whatwg.org/#concept-ipv4-serializer
static ErrorOr<String> serialize_ipv4_address(IPv4Address address)
{
    // 1. Let output be the empty string.
    // NOTE: Array to avoid prepend.
    Array<u8, 4> output;

    // 2. Let n be the value of address.
    u32 n = address;

    // 3. For each i in the range 1 to 4, inclusive:
    for (size_t i = 0; i <= 3; ++i) {
        // 1. Prepend n % 256, serialized, to output.
        output[3 - i] = n % 256;

        // 2. If i is not 4, then prepend U+002E (.) to output.
        // NOTE: done at end

        // 3. Set n to floor(n / 256).
        n /= 256;
    }

    // 4. Return output.
    return String::formatted("{}.{}.{}.{}", output[0], output[1], output[2], output[3]);
}

// https://url.spec.whatwg.org/#concept-ipv6-serializer
static void serialize_ipv6_address(IPv6Address const& address, StringBuilder& output)
{
    // 1. Let output be the empty string.

    // 2. Let compress be an index to the first IPv6 piece in the first longest sequences of address’s IPv6 pieces that are 0.
    Optional<size_t> compress;
    size_t longest_sequence_length = 0;
    size_t current_sequence_length = 0;
    size_t current_sequence_start = 0;
    for (size_t i = 0; i < 8; ++i) {
        if (address[i] == 0) {
            if (current_sequence_length == 0)
                current_sequence_start = i;
            ++current_sequence_length;
        } else {
            if (current_sequence_length > longest_sequence_length) {
                longest_sequence_length = current_sequence_length;
                compress = current_sequence_start;
            }
            current_sequence_length = 0;
        }
    }

    if (current_sequence_length > longest_sequence_length) {
        longest_sequence_length = current_sequence_length;
        compress = current_sequence_start;
    }

    // 3. If there is no sequence of address’s IPv6 pieces that are 0 that is longer than 1, then set compress to null.
    if (longest_sequence_length <= 1)
        compress = {};

    // 4. Let ignore0 be false.
    auto ignore0 = false;

    // 5. For each pieceIndex in the range 0 to 7, inclusive:
    for (size_t piece_index = 0; piece_index <= 7; ++piece_index) {
        // 1. If ignore0 is true and address[pieceIndex] is 0, then continue.
        if (ignore0 && address[piece_index] == 0)
            continue;

        // 2. Otherwise, if ignore0 is true, set ignore0 to false.
        if (ignore0)
            ignore0 = false;

        // 3. If compress is pieceIndex, then:
        if (compress == piece_index) {
            // 1. Let separator be "::" if pieceIndex is 0, and U+003A (:) otherwise.
            auto separator = piece_index == 0 ? "::"sv : ":"sv;

            // 2. Append separator to output.
            output.append(separator);

            // 3. Set ignore0 to true and continue.
            ignore0 = true;
            continue;
        }

        // 4. Append address[pieceIndex], represented as the shortest possible lowercase hexadecimal number, to output.
        output.appendff("{:x}", address[piece_index]);

        // 5. If pieceIndex is not 7, then append U+003A (:) to output.
        if (piece_index != 7)
            output.append(':');
    }

    // 6. Return output.
}

// https://url.spec.whatwg.org/#concept-ipv6-parser
static Optional<IPv6Address> parse_ipv6_address(StringView input)
{
    // 1. Let address be a new IPv6 address whose IPv6 pieces are all 0.
    Array<u16, 8> address {};

    // 2. Let pieceIndex be 0.
    size_t piece_index = 0;

    // 3. Let compress be null.
    Optional<size_t> compress;

    Vector<u32> code_points;
    code_points.ensure_capacity(input.length());
    for (auto code_point : Utf8View { input }) {
        code_points.append(code_point);
    }

    // 4. Let pointer be a pointer for input.
    size_t pointer = 0;
    auto c = [&]() -> u32 {
        if (pointer >= code_points.size())
            return end_of_file;
        return code_points[pointer];
    };

    auto remaining = [&]() -> ReadonlySpan<u32> {
        if ((pointer + 1) >= code_points.size())
            return {};
        return code_points.span().slice(pointer + 1);
    };

    // 5. If c is U+003A (:), then:
    if (c() == ':') {
        // 1. If remaining does not start with U+003A (:), IPv6-invalid-compression validation error, return failure.
        if (remaining().is_empty() || remaining()[0] != ':') {
            report_validation_error();
            return {};
        }

        // 2. Increase pointer by 2.
        pointer += 2;

        // 3. Increase pieceIndex by 1 and then set compress to pieceIndex.
        ++piece_index;
        compress = piece_index;
    }

    // 6. While c is not the EOF code point:
    while (c() != end_of_file) {
        // 1. If pieceIndex is 8, IPv6-too-many-pieces validation error, return failure.
        if (piece_index == 8) {
            report_validation_error();
            return {};
        }

        // 2. If c is U+003A (:), then:
        if (c() == ':') {
            // 1. If compress is non-null, IPv6-multiple-compression validation error, return failure.
            if (compress.has_value()) {
                report_validation_error();
                return {};
            }

            // 2. Increase pointer and pieceIndex by 1, set compress to pieceIndex, and then continue.
            ++pointer;
            ++piece_index;
            compress = piece_index;
            continue;
        }

        // 3. Let value and length be 0.
        u32 value = 0;
        size_t length = 0;

        // 4. While length is less than 4 and c is an ASCII hex digit,
        //    set value to value × 0x10 + c interpreted as hexadecimal number,
        //    and increase pointer and length by 1.
        while (length < 4 && is_ascii_hex_digit(c())) {
            value = value * 0x10 + parse_ascii_hex_digit(c());
            ++pointer;
            ++length;
        }

        // 5. If c is U+002E (.), then:
        if (c() == '.') {
            // 1. If length is 0, IPv4-in-IPv6-invalid-code-point validation error, return failure.
            if (length == 0) {
                report_validation_error();
                return {};
            }

            // 2. Decrease pointer by length.
            pointer -= length;

            // 3. If pieceIndex is greater than 6, IPv4-in-IPv6-too-many-pieces validation error, return failure.
            if (piece_index > 6) {
                report_validation_error();
                return {};
            }

            // 4. Let numbersSeen be 0.
            size_t numbers_seen = 0;

            // 5. While c is not the EOF code point:
            while (c() != end_of_file) {
                // 1. Let ipv4Piece be null.
                Optional<u32> ipv4_piece;

                // 2. If numbersSeen is greater than 0, then:
                if (numbers_seen > 0) {
                    // 1. If c is a U+002E (.) and numbersSeen is less than 4, then increase pointer by 1.
                    if (c() == '.' && numbers_seen < 4) {
                        ++pointer;
                    }
                    // 2. Otherwise, IPv4-in-IPv6-invalid-code-point validation error, return failure.
                    else {
                        report_validation_error();
                        return {};
                    }
                }

                // 3. If c is not an ASCII digit, IPv4-in-IPv6-invalid-code-point validation error, return failure.
                if (!is_ascii_digit(c())) {
                    report_validation_error();
                    return {};
                }

                // 4. While c is an ASCII digit:
                while (is_ascii_digit(c())) {
                    // 1. Let number be c interpreted as decimal number.
                    u32 number = parse_ascii_digit(c());

                    // 2. If ipv4Piece is null, then set ipv4Piece to number.
                    if (!ipv4_piece.has_value()) {
                        ipv4_piece = number;
                    }
                    // Otherwise, if ipv4Piece is 0, IPv4-in-IPv6-invalid-code-point validation error, return failure.
                    else if (ipv4_piece.value() == 0) {
                        report_validation_error();
                        return {};
                    }
                    // Otherwise, set ipv4Piece to ipv4Piece × 10 + number.
                    else {
                        ipv4_piece = ipv4_piece.value() * 10 + number;
                    }

                    // 3. If ipv4Piece is greater than 255, IPv4-in-IPv6-out-of-range-part validation error, return failure.
                    if (ipv4_piece.value() > 255) {
                        report_validation_error();
                        return {};
                    }

                    // 4. Increase pointer by 1.
                    ++pointer;
                }
                // 5. Set address[pieceIndex] to address[pieceIndex] × 0x100 + ipv4Piece.
                address[piece_index] = address[piece_index] * 0x100 + ipv4_piece.value();

                // 6. Increase numbersSeen by 1.
                ++numbers_seen;

                // 7. If numbersSeen is 2 or 4, then increase pieceIndex by 1.
                if (numbers_seen == 2 || numbers_seen == 4)
                    ++piece_index;
            }

            // 6. If numbersSeen is not 4, IPv4-in-IPv6-too-few-parts validation error, return failure.
            if (numbers_seen != 4) {
                report_validation_error();
                return {};
            }

            // 7. Break.
            break;
        }
        // 6. Otherwise, if c is U+003A (:):
        else if (c() == ':') {
            // 1. Increase pointer by 1.
            ++pointer;

            // 2. If c is the EOF code point, IPv6-invalid-code-point validation error, return failure.
            if (c() == end_of_file) {
                report_validation_error();
                return {};
            }
        }

        // 7. Otherwise, if c is not the EOF code point, IPv6-invalid-code-point validation error, return failure.
        else if (c() != end_of_file) {
            report_validation_error();
            return {};
        }

        // 8. Set address[pieceIndex] to value.
        address[piece_index] = value;

        // 9. Increase pieceIndex by 1.
        ++piece_index;
    }

    // 7. If compress is non-null, then:
    if (compress.has_value()) {
        // 1. Let swaps be pieceIndex − compress.
        size_t swaps = piece_index - compress.value();

        // 2. Set pieceIndex to 7.
        piece_index = 7;

        // 3. While pieceIndex is not 0 and swaps is greater than 0,
        //    swap address[pieceIndex] with address[compress + swaps − 1],
        //    and then decrease both pieceIndex and swaps by 1.
        while (piece_index != 0 && swaps > 0) {
            swap(address[piece_index], address[compress.value() + swaps - 1]);
            --piece_index;
            --swaps;
        }
    }

    // 8. Otherwise, if compress is null and pieceIndex is not 8, IPv6-too-few-pieces validation error, return failure.
    else if (!compress.has_value() && piece_index != 8) {
        report_validation_error();
        return {};
    }

    // 9. Return address.
    return address;
}

// https://url.spec.whatwg.org/#ends-in-a-number-checker
static bool ends_in_a_number_checker(StringView input)
{
    // 1. Let parts be the result of strictly splitting input on U+002E (.).
    auto parts = input.split_view("."sv, SplitBehavior::KeepEmpty);

    // 2. If the last item in parts is the empty string, then:
    if (parts.last().is_empty()) {
        // 1. If parts’s size is 1, then return false.
        if (parts.size() == 1)
            return false;

        // 2. Remove the last item from parts.
        parts.take_last();
    }

    // 3. Let last be the last item in parts.
    auto last = parts.last();

    // 4. If last is non-empty and contains only ASCII digits, then return true.
    if (!last.is_empty() && all_of(last, is_ascii_digit))
        return true;

    // 5. If parsing last as an IPv4 number does not return failure, then return true.
    // NOTE: This is equivalent to checking that last is "0X" or "0x", followed by zero or more ASCII hex digits.
    if (last.starts_with("0x"sv, CaseSensitivity::CaseInsensitive) && all_of(last.substring_view(2), is_ascii_hex_digit))
        return true;

    // 6. Return false.
    return false;
}

// https://url.spec.whatwg.org/#concept-domain-to-ascii
static ErrorOr<String> domain_to_ascii(StringView domain, bool be_strict)
{
    // 1. Let result be the result of running Unicode ToASCII with domain_name set to domain, UseSTD3ASCIIRules set to beStrict, CheckHyphens set to false, CheckBidi set to true, CheckJoiners set to true, Transitional_Processing set to false, and VerifyDnsLength set to beStrict. [UTS46]
    // 2. If result is a failure value, domain-to-ASCII validation error, return failure.

    // OPTIMIZATION: If beStrict is false, domain is an ASCII string, and strictly splitting domain on U+002E (.)
    //               does not produce any item that starts with an ASCII case-insensitive match for "xn--", this
    //               step is equivalent to ASCII lowercasing domain.
    if (!be_strict && all_of(domain, is_ascii)) {
        // 3. If result is the empty string, domain-to-ASCII validation error, return failure.
        if (domain.is_empty())
            return Error::from_string_literal("Empty domain");

        bool slow_path = false;
        for (auto part : domain.split_view('.')) {
            if (part.starts_with("xn--"sv, CaseSensitivity::CaseInsensitive)) {
                slow_path = true;
                break;
            }
        }

        if (!slow_path) {
            auto lowercase_domain = domain.to_lowercase_string();
            return String::from_utf8_without_validation(lowercase_domain.bytes());
        }
    }

    Unicode::IDNA::ToAsciiOptions const options {
        Unicode::IDNA::CheckHyphens::No,
        Unicode::IDNA::CheckBidi::Yes,
        Unicode::IDNA::CheckJoiners::Yes,
        be_strict ? Unicode::IDNA::UseStd3AsciiRules::Yes : Unicode::IDNA::UseStd3AsciiRules::No,
        Unicode::IDNA::TransitionalProcessing::No,
        be_strict ? Unicode::IDNA::VerifyDnsLength::Yes : Unicode::IDNA::VerifyDnsLength::No
    };
    auto result = TRY(Unicode::IDNA::to_ascii(Utf8View(domain), options));

    // 3. If result is the empty string, domain-to-ASCII validation error, return failure.
    if (result.is_empty())
        return Error::from_string_literal("Empty domain");

    // 4. Return result.
    return result;
}

// https://url.spec.whatwg.org/#concept-host-parser
static Optional<Host> parse_host(StringView input, bool is_opaque = false)
{
    // 1. If input starts with U+005B ([), then:
    if (input.starts_with('[')) {
        // 1. If input does not end with U+005D (]), IPv6-unclosed validation error, return failure.
        if (!input.ends_with(']')) {
            report_validation_error();
            return {};
        }

        // 2. Return the result of IPv6 parsing input with its leading U+005B ([) and trailing U+005D (]) removed.
        auto address = parse_ipv6_address(input.substring_view(1, input.length() - 2));
        if (!address.has_value())
            return {};
        return address.release_value();
    }

    // 2. If isOpaque is true, then return the result of opaque-host parsing input.
    if (is_opaque)
        return parse_opaque_host(input);

    // 3. Assert: input is not the empty string.
    VERIFY(!input.is_empty());

    // FIXME: 4. Let domain be the result of running UTF-8 decode without BOM on the percent-decoding of input.
    auto domain = percent_decode(input);

    // 5. Let asciiDomain be the result of running domain to ASCII with domain and false.
    auto ascii_domain_or_error = domain_to_ascii(domain, false);

    // 6. If asciiDomain is failure, then return failure.
    if (ascii_domain_or_error.is_error())
        return {};

    auto ascii_domain = ascii_domain_or_error.release_value();

    // 7. If asciiDomain contains a forbidden domain code point, domain-invalid-code-point validation error, return failure.
    for (auto character : ascii_domain.bytes_as_string_view()) {
        if (is_forbidden_domain_code_point(character)) {
            report_validation_error();
            return {};
        }
    }

    // 8. If asciiDomain ends in a number, then return the result of IPv4 parsing asciiDomain.
    if (ends_in_a_number_checker(ascii_domain)) {
        auto ipv4_host = parse_ipv4_address(ascii_domain);
        if (!ipv4_host.has_value())
            return {};

        return ipv4_host.release_value();
    }

    // 9. Return asciiDomain.
    return ascii_domain;
}

// https://url.spec.whatwg.org/#concept-host-serializer
ErrorOr<String> Parser::serialize_host(Host const& host)
{
    // 1. If host is an IPv4 address, return the result of running the IPv4 serializer on host.
    if (host.has<IPv4Address>())
        return serialize_ipv4_address(host.get<IPv4Address>());

    // 2. Otherwise, if host is an IPv6 address, return U+005B ([), followed by the result of running the IPv6 serializer on host, followed by U+005D (]).
    if (host.has<IPv6Address>()) {
        StringBuilder output;
        TRY(output.try_append('['));
        serialize_ipv6_address(host.get<IPv6Address>(), output);
        TRY(output.try_append(']'));
        return output.to_string();
    }

    // 3. Otherwise, host is a domain, opaque host, or empty host, return host.
    if (host.has<String>())
        return host.get<String>();
    return String {};
}

// https://url.spec.whatwg.org/#start-with-a-windows-drive-letter
constexpr bool starts_with_windows_drive_letter(StringView input)
{
    if (input.length() < 2)
        return false;
    if (!is_ascii_alpha(input[0]) || !(input[1] == ':' || input[1] == '|'))
        return false;
    if (input.length() == 2)
        return true;
    return "/\\?#"sv.contains(input[2]);
}

constexpr bool is_windows_drive_letter(StringView input)
{
    return input.length() == 2 && is_ascii_alpha(input[0]) && (input[1] == ':' || input[1] == '|');
}

constexpr bool is_normalized_windows_drive_letter(StringView input)
{
    return input.length() == 2 && is_ascii_alpha(input[0]) && input[1] == ':';
}

constexpr bool is_single_dot_path_segment(StringView input)
{
    return input == "."sv || input.equals_ignoring_ascii_case("%2e"sv);
}

constexpr bool is_double_dot_path_segment(StringView input)
{
    return input == ".."sv || input.equals_ignoring_ascii_case(".%2e"sv) || input.equals_ignoring_ascii_case("%2e."sv) || input.equals_ignoring_ascii_case("%2e%2e"sv);
}

// https://url.spec.whatwg.org/#shorten-a-urls-path
void Parser::shorten_urls_path(URL& url)
{
    // 1. Assert: url does not have an opaque path.
    VERIFY(!url.cannot_be_a_base_url());

    // 2. Let path be url’s path.
    auto& path = url.m_data->paths;

    // 3. If url’s scheme is "file", path’s size is 1, and path[0] is a normalized Windows drive letter, then return.
    if (url.scheme() == "file" && path.size() == 1 && is_normalized_windows_drive_letter(path[0]))
        return;

    // 4. Remove path’s last item, if any.
    if (!path.is_empty())
        path.take_last();
}

// https://url.spec.whatwg.org/#string-percent-encode-after-encoding
String Parser::percent_encode_after_encoding(TextCodec::Encoder& encoder, StringView input, PercentEncodeSet percent_encode_set, bool space_as_plus)
{
    // 1. Let encodeOutput be an empty I/O queue.
    StringBuilder output;

    // 2. Set potentialError to the result of running encode or fail with inputQueue, encoder, and encodeOutput.
    MUST(encoder.process(
        Utf8View(input),

        // 3. For each byte of encodeOutput converted to a byte sequence:
        [&](u8 byte) -> ErrorOr<void> {
            // 1. If spaceAsPlus is true and byte is 0x20 (SP), then append U+002B (+) to output and continue.
            if (space_as_plus && byte == ' ') {
                output.append('+');
                return {};
            }

            // 2. Let isomorph be a code point whose value is byte’s value.
            u32 isomorph = byte;

            // 3. Assert: percentEncodeSet includes all non-ASCII code points.

            // 4. If isomorphic is not in percentEncodeSet, then append isomorph to output.
            if (!code_point_is_in_percent_encode_set(isomorph, percent_encode_set)) {
                output.append_code_point(isomorph);
            }

            // 5. Otherwise, percent-encode byte and append the result to output.
            else {
                output.appendff("%{:02X}", byte);
            }

            return {};
        },

        // 4. If potentialError is non-null, then append "%26%23", followed by the shortest sequence of ASCII digits
        //    representing potentialError in base ten, followed by "%3B", to output.
        [&](u32 error) -> ErrorOr<void> {
            output.appendff("%26%23{}%3B", error);
            return {};
        }));

    // 6. Return output.
    return MUST(output.to_string());
}

// https://url.spec.whatwg.org/#concept-basic-url-parser
URL Parser::basic_parse(StringView raw_input, Optional<URL> const& base_url, URL* url, Optional<State> state_override, Optional<StringView> encoding)
{
    dbgln_if(URL_PARSER_DEBUG, "URL::Parser::basic_parse: Parsing '{}'", raw_input);

    size_t start_index = 0;
    size_t end_index = raw_input.length();

    // 1. If url is not given:
    auto url_buffer = URL();
    if (!url) {
        // 1. Set url to a new URL.
        url = &url_buffer;

        // 2. If input contains any leading or trailing C0 control or space, invalid-URL-unit validation error.
        // 3. Remove any leading and trailing C0 control or space from input.
        bool has_validation_error = false;

        for (; start_index < raw_input.length(); ++start_index) {
            if (!is_ascii_c0_control_or_space(raw_input[start_index]))
                break;
            has_validation_error = true;
        }

        for (; end_index > start_index; --end_index) {
            if (!is_ascii_c0_control_or_space(raw_input[end_index - 1]))
                break;
            has_validation_error = true;
        }

        if (has_validation_error)
            report_validation_error();
    }

    ByteString processed_input = raw_input.substring_view(start_index, end_index - start_index);

    // 2. If input contains any ASCII tab or newline, invalid-URL-unit validation error.
    // 3. Remove all ASCII tab or newline from input.
    for (auto const ch : processed_input) {
        if (ch == '\t' || ch == '\n' || ch == '\r') {
            report_validation_error();
            processed_input = processed_input.replace("\t"sv, ""sv, ReplaceMode::All).replace("\n"sv, ""sv, ReplaceMode::All).replace("\r"sv, ""sv, ReplaceMode::All);
            break;
        }
    }

    // 4. Let state be state override if given, or scheme start state otherwise.
    State state = state_override.value_or(State::SchemeStart);

    // 5. Set encoding to the result of getting an output encoding from encoding.
    Optional<TextCodec::Encoder&> encoder = {};
    if (encoding.has_value())
        encoder = TextCodec::encoder_for(TextCodec::get_output_encoding(*encoding));
    if (!encoder.has_value())
        encoder = TextCodec::encoder_for("utf-8"sv);
    VERIFY(encoder.has_value());

    // 6. Let buffer be the empty string.
    StringBuilder buffer;

    // 7. Let atSignSeen, insideBrackets, and passwordTokenSeen be false.
    bool at_sign_seen = false;
    bool inside_brackets = false;
    bool password_token_seen = false;

    Utf8View input(processed_input);

    // 8. Let pointer be a pointer for input.
    Utf8CodePointIterator iterator = input.begin();

    auto get_remaining = [&input, &iterator] {
        return input.substring_view(iterator - input.begin() + iterator.underlying_code_point_length_in_bytes()).as_string();
    };

    auto remaining_starts_with_two_ascii_hex_digits = [&]() {
        return is_ascii_hex_digit(iterator.peek(1).value_or(end_of_file)) && is_ascii_hex_digit(iterator.peek(2).value_or(end_of_file));
    };

    // 9. Keep running the following state machine by switching on state. If after a run pointer points to the EOF code point, go to the next step. Otherwise, increase pointer by 1 and continue with the state machine.
    // NOTE: "continue" should only be used to prevent incrementing the iterator, as this is done at the end of the loop.
    //       ++iterator : "increase pointer by 1"
    //       continue   : "decrease pointer by 1"
    for (;;) {
        u32 code_point = end_of_file;
        if (!iterator.done())
            code_point = *iterator;

        if constexpr (URL_PARSER_DEBUG) {
            if (code_point == end_of_file)
                dbgln("URL::Parser::basic_parse: {} state with EOF.", state_name(state));
            else if (is_ascii_printable(code_point))
                dbgln("URL::Parser::basic_parse: {} state with code point U+{:04X} ({:c}).", state_name(state), code_point, code_point);
            else
                dbgln("URL::Parser::basic_parse: {} state with code point U+{:04X}.", state_name(state), code_point);
        }

        switch (state) {
        // -> scheme start state, https://url.spec.whatwg.org/#scheme-start-state
        case State::SchemeStart:
            // 1. If c is an ASCII alpha, append c, lowercased, to buffer, and set state to scheme state.
            if (is_ascii_alpha(code_point)) {
                buffer.append_as_lowercase(code_point);
                state = State::Scheme;
            }
            // 2. Otherwise, if state override is not given, set state to no scheme state and decrease pointer by 1.
            else if (!state_override.has_value()) {
                state = State::NoScheme;
                continue;
            }
            // 3. Otherwise, return failure.
            else {
                return {};
            }
            break;
        // -> scheme state, https://url.spec.whatwg.org/#scheme-state
        case State::Scheme:
            // 1. If c is an ASCII alphanumeric, U+002B (+), U+002D (-), or U+002E (.), append c, lowercased, to buffer.
            if (is_ascii_alphanumeric(code_point) || code_point == '+' || code_point == '-' || code_point == '.') {
                buffer.append_as_lowercase(code_point);
            }
            // 2. Otherwise, if c is U+003A (:), then:
            else if (code_point == ':') {
                // 1. If state override is given, then:
                if (state_override.has_value()) {
                    // 1. If url’s scheme is a special scheme and buffer is not a special scheme, then return.
                    if (is_special_scheme(url->scheme()) && !is_special_scheme(buffer.string_view()))
                        return *url;

                    // 2. If url’s scheme is not a special scheme and buffer is a special scheme, then return.
                    if (!is_special_scheme(url->scheme()) && is_special_scheme(buffer.string_view()))
                        return *url;

                    // 3. If url includes credentials or has a non-null port, and buffer is "file", then return.
                    if ((url->includes_credentials() || url->port().has_value()) && buffer.string_view() == "file"sv)
                        return *url;

                    // 4. If url’s scheme is "file" and its host is an empty host, then return.
                    if (url->scheme() == "file"sv && url->host() == String {})
                        return *url;
                }

                // 2. Set url’s scheme to buffer.
                url->m_data->scheme = buffer.to_string_without_validation();

                // 3. If state override is given, then:
                if (state_override.has_value()) {
                    // 1. If url’s port is url’s scheme’s default port, then set url’s port to null.
                    if (url->port() == default_port_for_scheme(url->scheme()))
                        url->m_data->port = {};

                    // 2. Return.
                    return *url;
                }

                // 4. Set buffer to the empty string.
                buffer.clear();

                // 5. If url’s scheme is "file", then:
                if (url->scheme() == "file") {
                    // 1. If remaining does not start with "//", special-scheme-missing-following-solidus validation error.
                    if (!get_remaining().starts_with("//"sv)) {
                        report_validation_error();
                    }
                    // 2. Set state to file state.
                    state = State::File;
                }
                // 6. Otherwise, if url is special, base is non-null, and base’s scheme is url’s scheme:
                else if (url->is_special() && base_url.has_value() && base_url->scheme() == url->m_data->scheme) {
                    // 1. Assert: base is is special (and therefore does not have an opaque path).
                    VERIFY(base_url->is_special());

                    // 2. Set state to special relative or authority state.
                    state = State::SpecialRelativeOrAuthority;
                }
                // 7. Otherwise, if url is special, set state to special authority slashes state.
                else if (url->is_special()) {
                    state = State::SpecialAuthoritySlashes;
                }
                // 8. Otherwise, if remaining starts with an U+002F (/), set state to path or authority state and increase pointer by 1.
                else if (get_remaining().starts_with("/"sv)) {
                    state = State::PathOrAuthority;
                    ++iterator;
                }
                // 9. Otherwise, set url’s path to the empty string and set state to opaque path state.
                else {
                    url->m_data->cannot_be_a_base_url = true;
                    url->append_slash();
                    state = State::CannotBeABaseUrlPath;
                }
            }
            // 3. Otherwise, if state override is not given, set buffer to the empty string, state to no scheme state, and start over (from the first code point in input).
            else if (!state_override.has_value()) {
                buffer.clear();
                state = State::NoScheme;
                iterator = input.begin();
                continue;
            }
            // 4. Otherwise, return failure.
            else {
                return {};
            }
            break;
        // -> no scheme state, https://url.spec.whatwg.org/#no-scheme-state
        case State::NoScheme:
            // 1. If base is null, or base has an opaque path and c is not U+0023 (#), missing-scheme-non-relative-URL validation error, return failure.
            if (!base_url.has_value() || (base_url->m_data->cannot_be_a_base_url && code_point != '#')) {
                report_validation_error();
                return {};
            }
            // 2. Otherwise, if base has an opaque path and c is U+0023 (#), set url’s scheme to base’s scheme, url’s path to base’s path, url’s query
            //    to base’s query,url’s fragment to the empty string, and set state to fragment state.
            else if (base_url->m_data->cannot_be_a_base_url && code_point == '#') {
                url->m_data->scheme = base_url->m_data->scheme;
                url->m_data->paths = base_url->m_data->paths;
                url->m_data->query = base_url->m_data->query;
                url->m_data->fragment = String {};
                url->m_data->cannot_be_a_base_url = true;
                state = State::Fragment;
            }
            // 3. Otherwise, if base’s scheme is not "file", set state to relative state and decrease pointer by 1.
            else if (base_url->m_data->scheme != "file") {
                state = State::Relative;
                continue;
            }
            // 4. Otherwise, set state to file state and decrease pointer by 1.
            else {
                state = State::File;
                continue;
            }
            break;
        // -> special relative or authority state, https://url.spec.whatwg.org/#special-relative-or-authority-state
        case State::SpecialRelativeOrAuthority:
            // 1. If c is U+002F (/) and remaining starts with U+002F (/), then set state to special authority ignore slashes state and increase pointer by 1.
            if (code_point == '/' && get_remaining().starts_with("/"sv)) {
                state = State::SpecialAuthorityIgnoreSlashes;
                ++iterator;
            }
            // 2. Otherwise, special-scheme-missing-following-solidus validation error, set state to relative state and decrease pointer by 1.
            else {
                report_validation_error();
                state = State::Relative;
                continue;
            }
            break;
        // -> path or authority state, https://url.spec.whatwg.org/#path-or-authority-state
        case State::PathOrAuthority:
            // 1. If c is U+002F (/), then set state to authority state.
            if (code_point == '/') {
                state = State::Authority;
            }
            // 2. Otherwise, set state to path state, and decrease pointer by 1.
            else {
                state = State::Path;
                continue;
            }
            break;
        // -> relative state, https://url.spec.whatwg.org/#relative-state
        case State::Relative:
            // 1. Assert: base’s scheme is not "file".
            VERIFY(base_url->scheme() != "file");

            // 2. Set url’s scheme to base’s scheme.
            url->m_data->scheme = base_url->m_data->scheme;

            // 3. If c is U+002F (/), then set state to relative slash state.
            if (code_point == '/') {
                state = State::RelativeSlash;
            }
            // 4. Otherwise, if url is special and c is U+005C (\), invalid-reverse-solidus validation error, set state to relative slash state.
            else if (url->is_special() && code_point == '\\') {
                report_validation_error();
                state = State::RelativeSlash;
            }
            // 5. Otherwise:
            else {
                // 1. Set url’s username to base’s username, url’s password to base’s password, url’s host to base’s host, url’s port to base’s port, url’s path to a clone of base’s path, and url’s query to base’s query.
                url->m_data->username = base_url->m_data->username;
                url->m_data->password = base_url->m_data->password;
                url->m_data->host = base_url->m_data->host;
                url->m_data->port = base_url->m_data->port;
                url->m_data->paths = base_url->m_data->paths;
                url->m_data->query = base_url->m_data->query;

                // 2. If c is U+003F (?), then set url’s query to the empty string, and state to query state.
                if (code_point == '?') {
                    url->m_data->query = String {};
                    state = State::Query;
                }
                // 3. Otherwise, if c is U+0023 (#), set url’s fragment to the empty string and state to fragment state.
                else if (code_point == '#') {
                    url->m_data->fragment = String {};
                    state = State::Fragment;
                }
                // 4. Otherwise, if c is not the EOF code point:
                else if (code_point != end_of_file) {
                    // 1. Set url’s query to null.
                    url->m_data->query = {};

                    // 2. Shorten url’s path.
                    shorten_urls_path(*url);

                    // 3. Set state to path state and decrease pointer by 1.
                    state = State::Path;
                    continue;
                }
            }
            break;
        // -> relative slash state, https://url.spec.whatwg.org/#relative-slash-state
        case State::RelativeSlash:
            // 1. If url is special and c is U+002F (/) or U+005C (\), then:
            if (url->is_special() && (code_point == '/' || code_point == '\\')) {
                // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                if (code_point == '\\')
                    report_validation_error();

                // 2. Set state to special authority ignore slashes state.
                state = State::SpecialAuthorityIgnoreSlashes;
            }
            // 2. Otherwise, if c is U+002F (/), then set state to authority state.
            else if (code_point == '/') {
                state = State::Authority;
            }
            // 3. Otherwise, set url’s username to base’s username, url’s password to base’s password, url’s host to base’s host, url’s port to base’s port, state to path state, and then, decrease pointer by 1.
            else {
                url->m_data->username = base_url->m_data->username;
                url->m_data->password = base_url->m_data->password;
                url->m_data->host = base_url->m_data->host;
                url->m_data->port = base_url->m_data->port;
                state = State::Path;
                continue;
            }
            break;
        // -> special authority slashes state, https://url.spec.whatwg.org/#special-authority-slashes-state
        case State::SpecialAuthoritySlashes:
            // 1. If c is U+002F (/) and remaining starts with U+002F (/), then set state to special authority ignore slashes state and increase pointer by 1.
            if (code_point == '/' && get_remaining().starts_with("/"sv)) {
                state = State::SpecialAuthorityIgnoreSlashes;
                ++iterator;
            }
            // 2. Otherwise, special-scheme-missing-following-solidus validation error, set state to special authority ignore slashes state and decrease pointer by 1.
            else {
                report_validation_error();
                state = State::SpecialAuthorityIgnoreSlashes;
                continue;
            }
            break;
        // -> special authority ignore slashes state, https://url.spec.whatwg.org/#special-authority-ignore-slashes-state
        case State::SpecialAuthorityIgnoreSlashes:
            // 1. If c is neither U+002F (/) nor U+005C (\), then set state to authority state and decrease pointer by 1.
            if (code_point != '/' && code_point != '\\') {
                state = State::Authority;
                continue;
            }
            // 2. Otherwise, special-scheme-missing-following-solidus validation error.
            else {
                report_validation_error();
            }
            break;
        // -> authority state, https://url.spec.whatwg.org/#authority-state
        case State::Authority:
            // 1. If c is U+0040 (@), then:
            if (code_point == '@') {
                // 1. Invalid-credentials validation error.
                report_validation_error();

                // 2. If atSignSeen is true, then prepend "%40" to buffer.
                if (at_sign_seen) {
                    auto content = buffer.to_byte_string();
                    buffer.clear();
                    buffer.append("%40"sv);
                    buffer.append(content);
                }

                // 3. Set atSignSeen to true.
                at_sign_seen = true;

                StringBuilder username_builder;
                StringBuilder password_builder;

                // 4. For each codePoint in buffer:
                for (auto c : Utf8View(buffer.string_view())) {
                    // 1. If codePoint is U+003A (:) and passwordTokenSeen is false, then set passwordTokenSeen to true and continue.
                    if (c == ':' && !password_token_seen) {
                        password_token_seen = true;
                        continue;
                    }

                    // 2. Let encodedCodePoints be the result of running UTF-8 percent-encode codePoint using the userinfo percent-encode set.
                    // NOTE: This is done inside of step 3 and 4 implementation

                    // 3. If passwordTokenSeen is true, then append encodedCodePoints to url’s password.
                    if (password_token_seen) {
                        if (password_builder.is_empty())
                            password_builder.append(url->m_data->password);

                        append_percent_encoded_if_necessary(password_builder, c, PercentEncodeSet::Userinfo);
                    }
                    // 4. Otherwise, append encodedCodePoints to url’s username.
                    else {
                        if (username_builder.is_empty())
                            username_builder.append(url->m_data->username);

                        append_percent_encoded_if_necessary(username_builder, c, PercentEncodeSet::Userinfo);
                    }
                }

                if (username_builder.string_view().length() > url->m_data->username.bytes().size())
                    url->m_data->username = username_builder.to_string().release_value_but_fixme_should_propagate_errors();
                if (password_builder.string_view().length() > url->m_data->password.bytes().size())
                    url->m_data->password = password_builder.to_string().release_value_but_fixme_should_propagate_errors();

                // 5. Set buffer to the empty string.
                buffer.clear();

            }
            // 2. Otherwise, if one of the following is true:
            //    * c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#)
            //    * url is special and c is U+005C (\)
            else if ((code_point == end_of_file || code_point == '/' || code_point == '?' || code_point == '#')
                || (url->is_special() && code_point == '\\')) {
                // then:

                // 1. If atSignSeen is true and buffer is the empty string, host-missing validation error, return failure.
                if (at_sign_seen && buffer.is_empty()) {
                    report_validation_error();
                    return {};
                }

                // 2. Decrease pointer by buffer’s code point length + 1, set buffer to the empty string, and set state to host state.
                iterator = input.iterator_at_byte_offset(iterator - input.begin() - buffer.length() - 1);
                buffer.clear();
                state = State::Host;
            }
            // 3. Otherwise, append c to buffer.
            else {
                buffer.append_code_point(code_point);
            }
            break;
        // -> host state, https://url.spec.whatwg.org/#host-state
        // -> hostname state, https://url.spec.whatwg.org/#hostname-state
        case State::Host:
        case State::Hostname:
            // 1. If state override is given and url’s scheme is "file", then decrease pointer by 1 and set state to file host state.
            if (state_override.has_value() && url->scheme() == "file") {
                state = State::FileHost;
                continue;
            }

            // 2. Otherwise, if c is U+003A (:) and insideBrackets is false, then:
            if (code_point == ':' && !inside_brackets) {
                // 1. If buffer is the empty string, host-missing validation error, return failure.
                if (buffer.is_empty()) {
                    report_validation_error();
                    return {};
                }

                // 2. If state override is given and state override is hostname state, then return.
                if (state_override.has_value() && *state_override == State::Hostname)
                    return *url;

                // 3. Let host be the result of host parsing buffer with url is not special.
                auto host = parse_host(buffer.string_view(), !url->is_special());

                // 4. If host is failure, then return failure.
                if (!host.has_value())
                    return {};

                // 5. Set url’s host to host, buffer to the empty string, and state to port state.
                url->m_data->host = host.release_value();
                buffer.clear();
                state = State::Port;
            }
            // 3. Otherwise, if one of the following is true:
            //    * c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#)
            //    * url is special and c is U+005C (\)
            else if ((code_point == end_of_file || code_point == '/' || code_point == '?' || code_point == '#')
                || (url->is_special() && code_point == '\\')) {
                // then decrease pointer by 1, and then:
                // NOTE: pointer decrement is done by the continue below

                // 1. If url is special and buffer is the empty string, host-missing validation error, return failure.
                if (url->is_special() && buffer.is_empty()) {
                    report_validation_error();
                    return {};
                }

                // 2. Otherwise, if state override is given, buffer is the empty string, and either url includes credentials or url’s port is non-null, return.
                if (state_override.has_value() && buffer.is_empty() && (url->includes_credentials() || url->port().has_value()))
                    return *url;

                // 3. Let host be the result of host parsing buffer with url is not special.
                auto host = parse_host(buffer.string_view(), !url->is_special());

                // 4. If host is failure, then return failure.
                if (!host.has_value())
                    return {};

                // 5. Set url’s host to host, buffer to the empty string, and state to path start state.
                url->m_data->host = host.value();
                buffer.clear();
                state = State::Port;

                // 6. If state override is given, then return.
                if (state_override.has_value())
                    return *url;

                continue;

            }
            // 4. Otherwise:
            else {
                // 1. If c is U+005B ([), then set insideBrackets to true.
                if (code_point == '[') {
                    inside_brackets = true;
                }
                // 2. If c is U+005D (]), then set insideBrackets to false.
                else if (code_point == ']') {
                    inside_brackets = false;
                }

                // 3. Append c to buffer.
                buffer.append_code_point(code_point);
            }
            break;
        // -> port state, https://url.spec.whatwg.org/#port-state
        case State::Port:
            // 1. If c is an ASCII digit, append c to buffer.
            if (is_ascii_digit(code_point)) {
                buffer.append_code_point(code_point);
            }

            // 2. Otherwise, if one of the following is true:
            //    * c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#)
            //    * url is special and c is U+005C (\)
            //    * state override is given
            else if ((code_point == end_of_file || code_point == '/' || code_point == '?' || code_point == '#')
                || (url->is_special() && code_point == '\\')
                || state_override.has_value()) {
                // then:

                // 1. If buffer is not the empty string, then:
                if (!buffer.is_empty()) {
                    // 1. Let port be the mathematical integer value that is represented by buffer in radix-10 using ASCII digits for digits with values 0 through 9.
                    auto port = buffer.string_view().to_number<u16>();

                    // 2. If port is greater than 2^16 − 1, port-out-of-range validation error, return failure.
                    // NOTE: This is done by to_number.
                    if (!port.has_value()) {
                        report_validation_error();
                        return {};
                    }

                    // 3. Set url’s port to null, if port is url’s scheme’s default port; otherwise to port.
                    if (port.value() == default_port_for_scheme(url->scheme()))
                        url->m_data->port = {};
                    else
                        url->m_data->port = port.value();

                    // 4. Set buffer to the empty string.
                    buffer.clear();
                }

                // 2. If state override is given, then return.
                if (state_override.has_value())
                    return *url;

                // 3. Set state to path start state and decrease pointer by 1.
                state = State::PathStart;
                continue;
            }
            // 3. Otherwise, port-invalid validation error, return failure.
            else {
                report_validation_error();
                return {};
            }
            break;
        // -> file state, https://url.spec.whatwg.org/#file-state
        case State::File:
            // 1. Set url’s scheme to "file".
            url->m_data->scheme = "file"_string;

            // 2. Set url’s host to the empty string.
            url->m_data->host = String {};

            // 3. If c is U+002F (/) or U+005C (\), then:
            if (code_point == '/' || code_point == '\\') {
                // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                if (code_point == '\\')
                    report_validation_error();

                // 2. Set state to file slash state.
                state = State::FileSlash;
            }
            // 4. Otherwise, if base is non-null and base’s scheme is "file":
            else if (base_url.has_value() && base_url->m_data->scheme == "file") {
                // 1. Set url’s host to base’s host, url’s path to a clone of base’s path, and url’s query to base’s query.
                url->m_data->host = base_url->m_data->host;
                url->m_data->paths = base_url->m_data->paths;
                url->m_data->query = base_url->m_data->query;

                // 2. If c is U+003F (?), then set url’s query to the empty string and state to query state.
                if (code_point == '?') {
                    url->m_data->query = String {};
                    state = State::Query;
                }
                // 3. Otherwise, if c is U+0023 (#), set url’s fragment to the empty string and state to fragment state.
                else if (code_point == '#') {
                    url->m_data->fragment = String {};
                    state = State::Fragment;
                }
                // 4. Otherwise, if c is not the EOF code point:
                else if (code_point != end_of_file) {
                    // 1. Set url’s query to null.
                    url->m_data->query = {};

                    // 2. If the code point substring from pointer to the end of input does not start with a Windows drive letter, then shorten url’s path.
                    auto substring_from_pointer = input.substring_view(iterator - input.begin()).as_string();
                    if (!starts_with_windows_drive_letter(substring_from_pointer)) {
                        shorten_urls_path(*url);
                    }
                    // 3. Otherwise:
                    else {
                        // 1. File-invalid-Windows-drive-letter validation error.
                        report_validation_error();

                        // 2. Set url’s path to « ».
                        url->m_data->paths.clear();
                    }

                    // 4. Set state to path state and decrease pointer by 1.
                    state = State::Path;
                    continue;
                }
            }
            // 5. Otherwise, set state to path state, and decrease pointer by 1.
            else {
                state = State::Path;
                continue;
            }

            break;
        // -> file slash state, https://url.spec.whatwg.org/#file-slash-state
        case State::FileSlash:
            // 1. If c is U+002F (/) or U+005C (\), then:
            if (code_point == '/' || code_point == '\\') {
                // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                if (code_point == '\\')
                    report_validation_error();

                // 2. Set state to file host state.
                state = State::FileHost;
            }
            // 2. Otherwise:
            else {
                // 1. If base is non-null and base’s scheme is "file", then:
                if (base_url.has_value() && base_url->m_data->scheme == "file") {
                    // 1. Set url’s host to base’s host.
                    url->m_data->host = base_url->m_data->host;

                    // 2. If the code point substring from pointer to the end of input does not start with a Windows drive letter and base’s path[0] is a normalized Windows drive letter, then append base’s path[0] to url’s path.
                    auto substring_from_pointer = input.substring_view(iterator - input.begin()).as_string();
                    if (!starts_with_windows_drive_letter(substring_from_pointer) && is_normalized_windows_drive_letter(base_url->m_data->paths[0]))
                        url->m_data->paths.append(base_url->m_data->paths[0]);
                }

                // 2. Set state to path state, and decrease pointer by 1.
                state = State::Path;
                continue;
            }
            break;
        // -> file host state, https://url.spec.whatwg.org/#file-host-state
        case State::FileHost:
            // 1. If c is the EOF code point, U+002F (/), U+005C (\), U+003F (?), or U+0023 (#), then decrease pointer by 1 and then:
            //    NOTE: decreasing the pointer is done at the bottom of this block.
            if (code_point == end_of_file || code_point == '/' || code_point == '\\' || code_point == '?' || code_point == '#') {
                // 1. If state override is not given and buffer is a Windows drive letter, file-invalid-Windows-drive-letter-host validation error, set state to path state.
                if (!state_override.has_value() && is_windows_drive_letter(buffer.string_view())) {
                    report_validation_error();
                    state = State::Path;
                }
                // 2. Otherwise, if buffer is the empty string, then:
                else if (buffer.is_empty()) {
                    // 1. Set url’s host to the empty string.
                    url->m_data->host = String {};

                    // 2. If state override is given, then return.
                    if (state_override.has_value())
                        return *url;

                    // 3. Set state to path start state.
                    state = State::PathStart;
                }
                // 3. Otherwise, run these steps:
                else {
                    // 1. Let host be the result of host parsing buffer with url is not special.
                    auto host = parse_host(buffer.string_view(), !url->is_special());

                    // 2. If host is failure, then return failure.
                    if (!host.has_value())
                        return {};

                    // 3. If host is "localhost", then set host to the empty string.
                    if (host.value().has<String>() && host.value().get<String>() == "localhost"sv)
                        host = String {};

                    // 4. Set url’s host to host.
                    url->m_data->host = host.release_value();

                    // 5. If state override is given, then return.
                    if (state_override.has_value())
                        return *url;

                    // 6. Set buffer to the empty string and state to path start state.
                    buffer.clear();
                    state = State::PathStart;
                }

                // NOTE: Decrement specified at the top of this 'if' statement.
                continue;
            } else {
                buffer.append_code_point(code_point);
            }
            break;
        // -> path start state, https://url.spec.whatwg.org/#path-start-state
        case State::PathStart:
            // 1. If url is special, then:
            if (url->is_special()) {
                // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                if (code_point == '\\')
                    report_validation_error();

                // 2. Set state to path state.
                state = State::Path;

                // 3. If c is neither U+002F (/) nor U+005C (\), then decrease pointer by 1.
                if (code_point != '/' && code_point != '\\')
                    continue;
            }
            // 2. Otherwise, if state override is not given and c is U+003F (?), set url’s query to the empty string and state to query state.
            else if (!state_override.has_value() && code_point == '?') {
                url->m_data->query = String {};
                state = State::Query;
            }
            // 3. Otherwise, if state override is not given and c is U+0023 (#), set url’s fragment to the empty string and state to fragment state.
            else if (!state_override.has_value() && code_point == '#') {
                url->m_data->fragment = String {};
                state = State::Fragment;
            }
            // 4. Otherwise, if c is not the EOF code point:
            else if (code_point != end_of_file) {
                // 1. Set state to path state.
                state = State::Path;

                // 2. If c is not U+002F (/), then decrease pointer by 1.
                if (code_point != '/')
                    continue;
            }
            // 5. Otherwise, if state override is given and url’s host is null, append the empty string to url’s path.
            else if (state_override.has_value() && url->host().has<Empty>()) {
                url->append_slash();
            }
            break;
        // -> path state, https://url.spec.whatwg.org/#path-state
        case State::Path:
            // 1. If one of the following is true:
            //    * c is the EOF code point or U+002F (/)
            //    * url is special and c is U+005C (\)
            //    * state override is not given and c is U+003F (?) or U+0023 (#)
            if ((code_point == end_of_file || code_point == '/')
                || (url->is_special() && code_point == '\\')
                || (!state_override.has_value() && (code_point == '?' || code_point == '#'))) {
                // then:

                // 1. If url is special and c is U+005C (\), invalid-reverse-solidus validation error.
                if (url->is_special() && code_point == '\\')
                    report_validation_error();

                // 2. If buffer is a double-dot URL path segment, then:
                if (is_double_dot_path_segment(buffer.string_view())) {
                    // 1. Shorten url’s path.
                    shorten_urls_path(*url);

                    // 2. If neither c is U+002F (/), nor url is special and c is U+005C (\), append the empty string to url’s path.
                    if (code_point != '/' && !(url->is_special() && code_point == '\\'))
                        url->append_slash();
                }
                // 3. Otherwise, if buffer is a single-dot URL path segment and if neither c is U+002F (/), nor url is special and c is U+005C (\), append the empty string to url’s path.
                else if (is_single_dot_path_segment(buffer.string_view()) && code_point != '/' && !(url->is_special() && code_point == '\\')) {
                    url->append_slash();
                }
                // 4. Otherwise, if buffer is not a single-dot URL path segment, then:
                else if (!is_single_dot_path_segment(buffer.string_view())) {
                    // 1. If url’s scheme is "file", url’s path is empty, and buffer is a Windows drive letter, then replace the second code point in buffer with U+003A (:).
                    if (url->m_data->scheme == "file" && url->m_data->paths.is_empty() && is_windows_drive_letter(buffer.string_view())) {
                        auto drive_letter = buffer.string_view()[0];
                        buffer.clear();
                        buffer.append(drive_letter);
                        buffer.append(':');
                    }
                    // 2. Append buffer to url’s path.
                    url->m_data->paths.append(buffer.to_string_without_validation());
                }

                // 5. Set buffer to the empty string.
                buffer.clear();

                // 6. If c is U+003F (?), then set url’s query to the empty string and state to query state.
                if (code_point == '?') {
                    url->m_data->query = String {};
                    state = State::Query;
                }
                // 7. If c is U+0023 (#), then set url’s fragment to the empty string and state to fragment state.
                else if (code_point == '#') {
                    url->m_data->fragment = String {};
                    state = State::Fragment;
                }
            }
            // 2. Otherwise, run these steps
            else {
                // 1. If c is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
                if (!is_url_code_point(code_point) && code_point != '%')
                    report_validation_error();

                // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, validation error.
                if (code_point == '%' && !remaining_starts_with_two_ascii_hex_digits())
                    report_validation_error();

                // 3. UTF-8 percent-encode c using the path percent-encode set and append the result to buffer.
                append_percent_encoded_if_necessary(buffer, code_point, PercentEncodeSet::Path);
            }
            break;
        // -> opaque path state, https://url.spec.whatwg.org/#cannot-be-a-base-url-path-state
        case State::CannotBeABaseUrlPath:
            // NOTE: This does not follow the spec exactly but rather uses the buffer and only sets the path on EOF.
            VERIFY(url->m_data->paths.size() == 1 && url->m_data->paths[0].is_empty());

            // 1. If c is U+003F (?), then set url’s query to the empty string and state to query state.
            if (code_point == '?') {
                url->m_data->paths[0] = buffer.to_string_without_validation();
                url->m_data->query = String {};
                buffer.clear();
                state = State::Query;
            }
            // 2. Otherwise, if c is U+0023 (#), then set url’s fragment to the empty string and state to fragment state.
            else if (code_point == '#') {
                // NOTE: This needs to be percent decoded since the member variables contain decoded data.
                url->m_data->paths[0] = buffer.to_string_without_validation();
                url->m_data->fragment = String {};
                buffer.clear();
                state = State::Fragment;
            }
            // 3. Otherwise:
            else {
                // 1. If c is not the EOF code point, not a URL code point, and not U+0025 (%), invalid-URL-unit validation error.
                if (code_point != end_of_file && !is_url_code_point(code_point) && code_point != '%')
                    report_validation_error();

                // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, validation error.
                if (code_point == '%' && !remaining_starts_with_two_ascii_hex_digits())
                    report_validation_error();

                // 3. If c is not the EOF code point, UTF-8 percent-encode c using the C0 control percent-encode set and append the result to url’s path.
                if (code_point != end_of_file) {
                    append_percent_encoded_if_necessary(buffer, code_point, PercentEncodeSet::C0Control);
                } else {
                    url->m_data->paths[0] = buffer.to_string_without_validation();
                    buffer.clear();
                }
            }
            break;
        // -> query state, https://url.spec.whatwg.org/#query-state
        case State::Query:
            // 1. If encoding is not UTF-8 and one of the following is true:
            //     * url is not special
            //     * url’s scheme is "ws" or "wss"
            //  then set encoding to UTF-8.
            if (!url->is_special() || url->m_data->scheme == "ws" || url->m_data->scheme == "wss")
                encoder = TextCodec::encoder_for("utf-8"sv);

            // 2. If one of the following is true:
            //    * state override is not given and c is U+0023 (#)
            //    * c is the EOF code point
            if ((!state_override.has_value() && code_point == '#')
                || code_point == end_of_file) {
                // then:

                // 1. Let queryPercentEncodeSet be the special-query percent-encode set if url is special; otherwise the query percent-encode set.
                auto query_percent_encode_set = url->is_special() ? PercentEncodeSet::SpecialQuery : PercentEncodeSet::Query;

                // 2. Percent-encode after encoding, with encoding, buffer, and queryPercentEncodeSet, and append the result to url’s query.
                url->m_data->query = percent_encode_after_encoding(*encoder, buffer.string_view(), query_percent_encode_set);

                // 3. Set buffer to the empty string.
                buffer.clear();

                // 4. If c is U+0023 (#), then set url’s fragment to the empty string and state to fragment state.
                if (code_point == '#') {
                    url->m_data->fragment = String {};
                    state = State::Fragment;
                }
            }
            // 3. Otherwise, if c is not the EOF code point:
            else if (code_point != end_of_file) {
                // 1. If c is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
                if (!is_url_code_point(code_point) && code_point != '%')
                    report_validation_error();

                // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, validation error.
                if (code_point == '%' && !remaining_starts_with_two_ascii_hex_digits())
                    report_validation_error();

                // 3. Append c to buffer.
                buffer.append_code_point(code_point);
            }
            break;
        // -> fragment state, https://url.spec.whatwg.org/#fragment-state
        case State::Fragment:
            // NOTE: This does not follow the spec exactly but rather uses the buffer and only sets the fragment on EOF.
            // 1. If c is not the EOF code point, then:
            if (code_point != end_of_file) {
                // 1. If c is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
                if (!is_url_code_point(code_point) && code_point != '%')
                    report_validation_error();

                // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, validation error.
                if (code_point == '%' && !remaining_starts_with_two_ascii_hex_digits())
                    report_validation_error();

                // 3. UTF-8 percent-encode c using the fragment percent-encode set and append the result to url’s fragment.
                // NOTE: The percent-encode is done on EOF on the entire buffer.
                buffer.append_code_point(code_point);
            } else {
                url->m_data->fragment = percent_encode_after_encoding(*TextCodec::encoder_for("utf-8"sv), buffer.string_view(), PercentEncodeSet::Fragment);
                buffer.clear();
            }
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        if (iterator.done())
            break;
        ++iterator;
    }

    url->m_data->valid = true;
    dbgln_if(URL_PARSER_DEBUG, "URL::Parser::basic_parse: Parsed URL to be '{}'.", url->serialize());

    // 10. Return url.
    return *url;
}

}
