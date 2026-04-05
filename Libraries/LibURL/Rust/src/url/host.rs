/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::net::Ipv4Addr;
use std::net::Ipv6Addr;

use libunicode_rust::idna::ToAsciiOptions;
use libunicode_rust::idna::idna_to_ascii;

use super::Host;
use super::State;
use super::parser::report_validation_error;
use super::percent_encoding::PercentEncodeSet;
use super::percent_encoding::percent_decode;
use super::percent_encoding::percent_encode;

// https://url.spec.whatwg.org/#forbidden-host-code-point
fn is_forbidden_host_code_point(code_point: char) -> bool {
    // A forbidden host code point is U+0000 NULL, U+0009 TAB, U+000A LF, U+000D CR, U+0020 SPACE, U+0023 (#), U+002F (/),
    // U+003A (:), U+003C (<), U+003E (>), U+003F (?), U+0040 (@), U+005B ([), U+005C (\), U+005D (]), U+005E (^), or U+007C (|).
    "\0\t\n\r #/:<>?@[\\]^|".contains(code_point)
}

// https://url.spec.whatwg.org/#forbidden-domain-code-point
pub(super) fn is_forbidden_domain_code_point(code_point: char) -> bool {
    // A forbidden domain code point is a forbidden host code point, a C0 control, U+0025 (%), or U+007F DELETE.
    is_forbidden_host_code_point(code_point)
        || code_point.is_ascii_control()
        || code_point == '%'
        || code_point == '\u{007F}'
}

// https://url.spec.whatwg.org/#concept-opaque-host-parser
fn parse_opaque_host(input: &str) -> Option<Host> {
    // 1. If input contains a forbidden host code point, host-invalid-code-point validation error, return failure.
    if input.chars().any(is_forbidden_host_code_point) {
        report_validation_error(State::Host, 0, None, "host-invalid-code-point");
        return None;
    }

    // 2. If input contains a code point that is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
    // 3. If input contains a U+0025 (%) and the two code points following it are not ASCII hex digits, invalid-URL-unit validation error.
    // NOTE: These steps are not implemented because they are not cheap checks and exist just to report validation errors. With how we
    //       currently report validation errors, they are only useful for debugging efforts in the URL parsing code.

    // 4. Return the result of running UTF-8 percent-encode on input using the C0 control percent-encode set.
    Some(Host::Opaque(percent_encode(input, PercentEncodeSet::C0Control, false)))
}

// https://url.spec.whatwg.org/#concept-domain-to-ascii
pub(crate) fn domain_to_ascii(domain: &str, be_strict: bool) -> Option<String> {
    // 1. Let result be the result of running Unicode ToASCII with domain_name set to domain,
    //     CheckHyphens set to beStrict,
    //     CheckBidi set to true,
    //     CheckJoiners set to true,
    //     UseSTD3ASCIIRules set to beStrict,
    //     Transitional_Processing set to false,
    //     VerifyDnsLength set to beStrict,
    //     and IgnoreInvalidPunycode set to false. [UTS46]
    //
    // NOTE: If beStrict is false, domain is an ASCII string, and strictly splitting domain on U+002E (.) does not
    //       produce any item that starts with an ASCII case-insensitive match for "xn--", this step is equivalent to
    //       ASCII lowercasing domain.

    // OPTIMIZATION: See spec note above.
    if !be_strict && domain.is_ascii() {
        // 3. If result is the empty string, domain-to-ASCII validation error, return failure.
        if domain.is_empty() {
            report_validation_error(State::Host, 0, None, "domain-to-ASCII");
            return None;
        }

        let mut slow_path = false;

        for part in domain.split('.') {
            if part.len() >= 4 && part[..4].eq_ignore_ascii_case("xn--") {
                slow_path = true;
                break;
            }
        }

        if !slow_path {
            let result = domain.to_ascii_lowercase();
            return Some(result);
        }
    }

    // 2. If result is a failure value, domain-to-ASCII validation error, return failure.
    let result = idna_to_ascii(
        domain,
        ToAsciiOptions {
            check_hyphens: be_strict,
            check_bidi: true,
            check_joiners: true,
            use_std3_ascii_rules: be_strict,
            transitional_processing: false,
            verify_dns_length: be_strict,
            ignore_invalid_punycode: false,
        },
    )?;

    // 3. If beStrict is false:
    if !be_strict {
        // 1. If result is the empty string, domain-to-ASCII validation error, return failure.
        if result.is_empty() {
            report_validation_error(State::Host, 0, None, "domain-to-ASCII");
            return None;
        }

        // 2. If result contains a forbidden domain code point, domain-invalid-code-point validation error, return failure.
        // NOTE: Due to web compatibility and compatibility with non-DNS-based systems the forbidden domain code points
        //       are a subset of those disallowed when UseSTD3ASCIIRules is true. See also issue #397.
        if result.chars().any(is_forbidden_domain_code_point) {
            report_validation_error(State::Host, 0, None, "domain-invalid-code-point");
            return None;
        }
    }

    // 4. Assert: result is not the empty string and does not contain a forbidden domain code point.
    // NOTE: Unicode IDNA Compatibility Processing guarantees this holds when beStrict is true. [UTS46]
    assert!(!result.is_empty());
    assert!(!result.chars().any(is_forbidden_domain_code_point));

    // 5. Return result.
    // NOTE: This document and the web platform at large use Unicode IDNA Compatibility Processing and not IDNA2008. For
    //       instance, ☕.example becomes xn--53h.example and not failure. [UTS46] [RFC5890]
    Some(result)
}

struct ParsedIpv4Number {
    number: u32,
    validation_error: bool,
}

// https://url.spec.whatwg.org/#ipv4-number-parser
fn parse_ipv4_number(mut input: &str) -> Option<ParsedIpv4Number> {
    // 1. If input is the empty string, then return failure.
    if input.is_empty() {
        return None;
    }

    // 2. Let validationError be false.
    let mut validation_error = false;

    // 3. Let R be 10.
    let mut radix = 10;

    // 4. If input contains at least two code points and the first two code points are either "0X" or "0x", then:
    if input.len() >= 2 && (input.starts_with("0X") || input.starts_with("0x")) {
        // 1. Set validationError to true.
        validation_error = true;

        // 2. Remove the first two code points from input.
        input = &input[2..];

        // 3. Set R to 16.
        radix = 16;
    }
    // 5. Otherwise, if input contains at least two code points and the first code point is U+0030 (0), then:
    else if input.len() >= 2 && input.starts_with('0') {
        // 1. Set validationError to true.
        validation_error = true;

        // 2. Remove the first code point from input.
        input = &input[1..];

        // 3. Set R to 8.
        radix = 8;
    }

    // 6. If input is the empty string, then return (0, true).
    if input.is_empty() {
        return Some(ParsedIpv4Number {
            number: 0,
            validation_error: true,
        });
    }

    // 7. If input contains a code point that is not a radix-R digit, then return failure.
    // 8. Let output be the mathematical integer value that is represented by input in radix-R notation, using ASCII hex
    //    digits for digits with values 0 through 15.
    let number = u32::from_str_radix(input, radix).ok()?;

    // 9. Return (output, validationError).
    Some(ParsedIpv4Number {
        number,
        validation_error,
    })
}

// https://url.spec.whatwg.org/#concept-ipv4-parser
fn parse_ipv4_address(input: &str) -> Option<Ipv4Addr> {
    // 1. Let parts be the result of strictly splitting input on U+002E (.).
    let mut parts: Vec<&str> = input.split('.').collect();

    // 2. If the last item in parts is the empty string, then:
    if parts.last().unwrap().is_empty() {
        // 1. IPv4-empty-part validation error.
        report_validation_error(State::Host, 0, None, "IPv4-empty-part");

        // 2. If parts’s size is greater than 1, then remove the last item from parts.
        if parts.len() > 1 {
            parts.pop();
        }
    }

    // 3. If parts’s size is greater than 4, IPv4-too-many-parts validation error, return failure.
    if parts.len() > 4 {
        report_validation_error(State::Host, 0, None, "IPv4-too-many-parts");
        return None;
    }

    // 4. Let numbers be an empty list.
    let mut numbers = Vec::with_capacity(parts.len());

    // 5. For each part of parts:
    for part in parts {
        // 1. Let result be the result of parsing part.
        // 2. If result is failure, IPv4-non-numeric-part validation error, return failure.
        let result = parse_ipv4_number(part)?;

        // 3. If result[1] is true, IPv4-non-decimal-part validation error.
        if result.validation_error {
            report_validation_error(State::Host, 0, None, "IPv4-non-decimal-part");
        }

        // 4. Append result[0] to numbers.
        numbers.push(result.number);
    }

    // 6. If any item in numbers is greater than 255, IPv4-out-of-range-part validation error.
    // 7. If any but the last item in numbers is greater than 255, then return failure.
    for (index, number) in numbers.iter().copied().enumerate() {
        if number > 255 {
            report_validation_error(State::Host, 0, None, "IPv4-out-of-range-part");
            if index != numbers.len() - 1 {
                return None;
            }
        }
    }

    // 8. If the last item in numbers is greater than or equal to 256(5 − numbers’s size), then return failure.
    if u64::from(numbers.last().copied()?) >= 256u64.pow(5 - numbers.len() as u32) {
        return None;
    }

    // 9. Let ipv4 be the last item in numbers.
    // 10. Remove the last item from numbers.
    let mut ipv4 = numbers.pop().unwrap();

    // 11. Let counter be 0.
    // 12. For each n of numbers:
    for (counter, number) in numbers.into_iter().enumerate() {
        // 1. Increment ipv4 by n × 256(3 − counter).
        ipv4 += number * 256u32.pow(3 - counter as u32);

        // 2. Increment counter by 1.
    }

    // 13. Return ipv4.
    Some(Ipv4Addr::from(ipv4))
}

// https://url.spec.whatwg.org/#concept-ipv6-parser
fn parse_ipv6_address(input: &str) -> Option<Ipv6Addr> {
    // 1. Let address be a new IPv6 address whose pieces are all 0.
    let mut address = [0u16; 8];

    // 2. Let pieceIndex be 0.
    let mut piece_index = 0usize;

    // 3. Let compress be null.
    let mut compress = None;

    // 4. Let pointer be a pointer for input.
    let bytes = input.as_bytes();
    let mut pointer = 0usize;

    // 5. If c is U+003A (:), then:
    if bytes.get(pointer) == Some(&b':') {
        // 1. If remaining does not start with U+003A (:), IPv6-invalid-compression validation error, return failure.
        if bytes.get(pointer + 1) != Some(&b':') {
            report_validation_error(State::Host, 0, None, "IPv6-invalid-compression");
            return None;
        }

        // 2. Increase pointer by 2.
        pointer += 2;

        // 3. Increase pieceIndex by 1 and then set compress to pieceIndex.
        piece_index += 1;
        compress = Some(piece_index);
    }

    // 6. While c is not the EOF code point:
    while let Some(&code_point) = bytes.get(pointer) {
        // 1. If pieceIndex is 8, IPv6-too-many-pieces validation error, return failure.
        if piece_index == 8 {
            report_validation_error(State::Host, 0, None, "IPv6-too-many-pieces");
            return None;
        }

        // 2. If c is U+003A (:), then:
        if code_point == b':' {
            // 1. If compress is non-null, IPv6-multiple-compression validation error, return failure.
            if compress.is_some() {
                report_validation_error(State::Host, 0, None, "IPv6-multiple-compression");
                return None;
            }

            // 2. Increase pointer and pieceIndex by 1, set compress to pieceIndex, and then continue.
            pointer += 1;
            piece_index += 1;
            compress = Some(piece_index);
            continue;
        }

        // 3. Let value and length be 0.
        let mut value = 0u32;
        let mut length = 0usize;

        // 4. While length is less than 4 and c is an ASCII hex digit, set value to value × 0x10 + c interpreted as
        //     hexadecimal number, and increase pointer and length by 1.
        while length < 4 {
            let Some(&code_point) = bytes.get(pointer) else {
                break;
            };
            let Some(digit) = char::from(code_point).to_digit(16) else {
                break;
            };
            value = value * 0x10 + digit;
            pointer += 1;
            length += 1;
        }

        // 5. If c is U+002E (.), then:
        if bytes.get(pointer) == Some(&b'.') {
            // 1. If length is 0, IPv4-in-IPv6-invalid-code-point validation error, return failure.
            if length == 0 {
                report_validation_error(State::Host, 0, None, "IPv4-in-IPv6-invalid-code-point");
                return None;
            }

            // 2. Decrease pointer by length.
            pointer -= length;

            // 3. If pieceIndex is greater than 6, IPv4-in-IPv6-too-many-pieces validation error, return failure.
            if piece_index > 6 {
                report_validation_error(State::Host, 0, None, "IPv4-in-IPv6-too-many-pieces");
                return None;
            }

            // 4. Let numbersSeen be 0.
            let mut numbers_seen = 0usize;

            // 5. While c is not the EOF code point:
            while let Some(&code_point) = bytes.get(pointer) {
                // 1. Let ipv4Piece be null.
                let mut ipv4_piece = None;

                // 2. If numbersSeen is greater than 0, then:
                if numbers_seen > 0 {
                    // 1. If c is a U+002E (.) and numbersSeen is less than 4, then increase pointer by 1.
                    if code_point == b'.' && numbers_seen < 4 {
                        pointer += 1;
                    }
                    // 2. Otherwise, IPv4-in-IPv6-invalid-code-point validation error, return failure.
                    else {
                        report_validation_error(State::Host, 0, None, "IPv4-in-IPv6-invalid-code-point");
                        return None;
                    }
                }

                // 3. If c is not an ASCII digit, IPv4-in-IPv6-invalid-code-point validation error, return failure.
                let Some(&code_point) = bytes.get(pointer) else {
                    break;
                };
                if !code_point.is_ascii_digit() {
                    report_validation_error(State::Host, 0, None, "IPv4-in-IPv6-invalid-code-point");
                    return None;
                }

                // 4. While c is an ASCII digit:
                while let Some(&code_point) = bytes.get(pointer) {
                    if !code_point.is_ascii_digit() {
                        break;
                    }

                    // 1. Let number be c interpreted as decimal number.
                    let number = u32::from(code_point - b'0');

                    // 2. If ipv4Piece is null, then set ipv4Piece to number.
                    if ipv4_piece.is_none() {
                        ipv4_piece = Some(number);
                    }
                    // 3. Otherwise, if ipv4Piece is 0, IPv4-in-IPv6-invalid-code-point validation error, return failure.
                    else if ipv4_piece == Some(0) {
                        report_validation_error(State::Host, 0, None, "IPv4-in-IPv6-invalid-code-point");
                        return None;
                    }
                    // 4. Otherwise, set ipv4Piece to ipv4Piece × 10 + number.
                    else {
                        ipv4_piece = Some(ipv4_piece.unwrap() * 10 + number);
                    }

                    // 5. If ipv4Piece is greater than 255, IPv4-in-IPv6-out-of-range-part validation error, return failure.
                    if ipv4_piece.unwrap() > 255 {
                        report_validation_error(State::Host, 0, None, "IPv4-in-IPv6-out-of-range-part");
                        return None;
                    }

                    // 6. Increase pointer by 1.
                    pointer += 1;
                }

                // 5. Set address[pieceIndex] to address[pieceIndex] × 0x100 + ipv4Piece.
                address[piece_index] = address[piece_index] * 0x100 + ipv4_piece.unwrap() as u16;

                // 6. Increase numbersSeen by 1.
                numbers_seen += 1;

                // 7. If numbersSeen is 2 or 4, then increase pieceIndex by 1.
                if numbers_seen == 2 || numbers_seen == 4 {
                    piece_index += 1;
                }
            }

            // 6. If numbersSeen is not 4, IPv4-in-IPv6-too-few-parts validation error, return failure.
            if numbers_seen != 4 {
                report_validation_error(State::Host, 0, None, "IPv4-in-IPv6-too-few-parts");
                return None;
            }

            // 7. Break.
            break;
        }
        // 6. Otherwise, if c is U+003A (:):
        else if bytes.get(pointer) == Some(&b':') {
            // 1. Increase pointer by 1.
            pointer += 1;

            // 2. If c is the EOF code point, IPv6-invalid-code-point validation error, return failure.
            if bytes.get(pointer).is_none() {
                report_validation_error(State::Host, 0, None, "IPv6-invalid-code-point");
                return None;
            }
        }
        // 7. Otherwise, if c is not the EOF code point, IPv6-invalid-code-point validation error, return failure.
        else if bytes.get(pointer).is_some() {
            report_validation_error(State::Host, 0, None, "IPv6-invalid-code-point");
            return None;
        }

        // 8. Set address[pieceIndex] to value.
        address[piece_index] = value as u16;

        // 9. Increase pieceIndex by 1.
        piece_index += 1;
    }

    // 7. If compress is non-null, then:
    if let Some(compress) = compress {
        // 1. Let swaps be pieceIndex − compress.
        let mut swaps = piece_index - compress;

        // 2. Set pieceIndex to 7.
        piece_index = 7;

        // 3. While pieceIndex is not 0 and swaps is greater than 0, swap address[pieceIndex] with
        //    address[compress + swaps − 1], and then decrease both pieceIndex and swaps by 1.
        while piece_index != 0 && swaps > 0 {
            address.swap(piece_index, compress + swaps - 1);
            piece_index -= 1;
            swaps -= 1;
        }
    }
    // 8. Otherwise, if compress is null and pieceIndex is not 8, IPv6-too-few-pieces validation error, return failure.
    else if piece_index != 8 {
        report_validation_error(State::Host, 0, None, "IPv6-too-few-pieces");
        return None;
    }

    // 9. Return address.
    Some(Ipv6Addr::from(address))
}

// https://url.spec.whatwg.org/#ends-in-a-number-checker
fn ends_in_a_number_checker(input: &str) -> bool {
    // 1. Let parts be the result of strictly splitting input on U+002E (.).
    let mut parts: Vec<&str> = input.split('.').collect();

    // 2. If the last item in parts is the empty string, then:
    if parts.last().unwrap().is_empty() {
        // 1. If parts’s size is 1, then return false.
        if parts.len() == 1 {
            return false;
        }

        // 2. Remove the last item from parts.
        parts.pop();
    }

    // 3. Let last be the last item in parts.
    let last = parts.last().unwrap();

    // 4. If last is non-empty and contains only ASCII digits, then return true.
    // NOTE: The erroneous input "09" will be caught by the IPv4 parser at a later stage.
    if !last.is_empty() && last.chars().all(|code_point| code_point.is_ascii_digit()) {
        return true;
    }

    // 5. If parsing last as an IPv4 number does not return failure, then return true.
    // NOTE: This is equivalent to checking that last is "0X" or "0x", followed by zero or more ASCII hex digits.
    // 6. Return false.
    last.len() >= 2
        && last[..2].eq_ignore_ascii_case("0x")
        && last[2..].chars().all(|code_point| code_point.is_ascii_hexdigit())
}

// https://url.spec.whatwg.org/#concept-host-parser
pub(crate) fn parse_host(input: &str, is_opaque: bool) -> Option<Host> {
    // 1. If input starts with U+005B ([), then:
    if input.starts_with('[') {
        // 1. If input does not end with U+005D (]), IPv6-unclosed validation error, return failure.
        if !input.ends_with(']') {
            report_validation_error(State::Host, 0, None, "IPv6-unclosed");
            return None;
        }

        // 2. Return the result of IPv6 parsing input with its leading U+005B ([) and trailing U+005D (]) removed.
        let address = parse_ipv6_address(&input[1..input.len() - 1])?;
        return Some(Host::Ipv6(address));
    }

    // 2. If isOpaque is true, then return the result of opaque-host parsing input.
    if is_opaque {
        return parse_opaque_host(input);
    }

    // 3. Assert: input is not the empty string.
    assert!(!input.is_empty());

    // 4. Let domain be the result of running UTF-8 decode without BOM on the percent-decoding of input.
    let domain = String::from_utf8_lossy(&percent_decode(input)).into_owned();

    // 5. Let asciiDomain be the result of running domain to ASCII with domain and false.
    // 6. If asciiDomain is failure, then return failure.
    let ascii_domain = domain_to_ascii(&domain, false)?;

    // 7. If asciiDomain contains a forbidden domain code point, domain-invalid-code-point validation error, return failure.
    if ascii_domain.chars().any(is_forbidden_domain_code_point) {
        report_validation_error(State::Host, 0, None, "domain-invalid-code-point");
        return None;
    }

    // 8. If asciiDomain ends in a number, then return the result of IPv4 parsing asciiDomain.
    if ends_in_a_number_checker(&ascii_domain) {
        return parse_ipv4_address(&ascii_domain).map(Host::Ipv4);
    }

    // 9. Return asciiDomain.
    Some(Host::Domain(ascii_domain))
}
