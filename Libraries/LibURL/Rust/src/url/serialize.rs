/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::fmt::Write;
use std::net::Ipv4Addr;
use std::net::Ipv6Addr;

// https://url.spec.whatwg.org/#concept-ipv4-serializer
pub(super) fn serialize_ipv4_address(address: Ipv4Addr, output: &mut String) {
    write!(output, "{address}").unwrap();
}

// https://url.spec.whatwg.org/#find-the-ipv6-address-compressed-piece-index
fn find_the_ipv6_address_compressed_piece_index(address: Ipv6Addr) -> Option<usize> {
    let address = address.segments();

    // 1. Let longestIndex be null.
    let mut longest_index = None;

    // 2. Let longestSize be 1.
    let mut longest_size = 1;

    // 3. Let foundIndex be null.
    let mut found_index = None;

    // 4. Let foundSize be 0.
    let mut found_size = 0;

    // 5. For each pieceIndex of address’s pieces’s indices:
    for (piece_index, piece) in address.iter().enumerate() {
        // 1. If address’s pieces[pieceIndex] is not 0:
        if *piece != 0 {
            // 1. If foundSize is greater than longestSize, then set longestIndex to foundIndex and longestSize to foundSize.
            if found_size > longest_size {
                longest_index = found_index;
                longest_size = found_size;
            }

            // 2. Set foundIndex to null.
            found_index = None;

            // 3. Set foundSize to 0.
            found_size = 0;
        }
        // 2. Otherwise:
        else {
            // 1. If foundIndex is null, then set foundIndex to pieceIndex.
            if found_index.is_none() {
                found_index = Some(piece_index);
            }

            // 2. Increment foundSize by 1.
            found_size += 1;
        }
    }

    // 6. If foundSize is greater than longestSize, then return foundIndex.
    if found_size > longest_size {
        return found_index;
    }

    // 7. Return longestIndex.
    longest_index
}

// https://url.spec.whatwg.org/#concept-ipv6-serializer
pub(super) fn serialize_ipv6_address(address: Ipv6Addr, output: &mut String) {
    let address = address.segments();

    // 1. Let output be the empty string.

    // 2. Let compress be the result of finding the IPv6 address compressed piece index given address.
    let compress = find_the_ipv6_address_compressed_piece_index(Ipv6Addr::from(address));

    // 3. Let ignore0 be false.
    let mut ignore0 = false;

    // 4. For each pieceIndex of address’s pieces’s indices:
    for (piece_index, piece) in address.iter().enumerate() {
        // 1. If ignore0 is true and address[pieceIndex] is 0, then continue.
        if ignore0 && *piece == 0 {
            continue;
        }

        // 2. Otherwise, if ignore0 is true, set ignore0 to false.
        if ignore0 {
            ignore0 = false;
        }

        // 3. If compress is pieceIndex, then:
        if compress == Some(piece_index) {
            // 1. Let separator be "::" if pieceIndex is 0, and U+003A (:) otherwise.
            let separator = if piece_index == 0 { "::" } else { ":" };

            // 2. Append separator to output.
            output.push_str(separator);

            // 3. Set ignore0 to true and continue.
            ignore0 = true;
            continue;
        }

        // 4. Append address[pieceIndex], represented as the shortest possible lowercase hexadecimal number, to output.
        write!(output, "{piece:x}").unwrap();

        // 5. If pieceIndex is not 7, then append U+003A (:) to output.
        if piece_index != 7 {
            output.push(':');
        }
    }

    // 5. Return output.
}
