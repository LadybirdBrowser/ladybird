/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::net::Ipv4Addr;
use std::net::Ipv6Addr;

use super::parser::url_includes_credentials;
use super::types::ExcludeFragment;
use super::types::Host;
use super::types::Url;

// https://url.spec.whatwg.org/#concept-ipv4-serializer
fn serialize_ipv4_address(address: Ipv4Addr) -> String {
    address.to_string()
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
fn serialize_ipv6_address(address: Ipv6Addr, output: &mut String) {
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
        output.push_str(&format!("{piece:x}"));

        // 5. If pieceIndex is not 7, then append U+003A (:) to output.
        if piece_index != 7 {
            output.push(':');
        }
    }

    // 5. Return output.
}

impl Host {
    // https://url.spec.whatwg.org/#concept-host-serializer
    pub(crate) fn serialize(&self) -> String {
        match self {
            // 1. If host is an IPv4 address, return the result of running the IPv4 serializer on host.
            Self::Ipv4(address) => serialize_ipv4_address(*address),
            // 2. Otherwise, if host is an IPv6 address, return U+005B ([), followed by the result of running the
            //    IPv6 serializer on host, followed by U+005D (]).
            Self::Ipv6(address) => {
                let mut output = String::new();
                output.push('[');
                serialize_ipv6_address(*address, &mut output);
                output.push(']');
                output
            }
            // 3. Otherwise, host is a domain, opaque host, or empty host, return host.
            Self::Domain(string) | Self::Opaque(string) => string.clone(),
        }
    }
}

impl Url {
    // https://url.spec.whatwg.org/#url-path-serializer
    pub(crate) fn serialize_path(&self) -> String {
        // 1. If url has an opaque path, then return url's path.
        if self.has_opaque_path {
            return self.path[0].clone();
        }

        // 2. Let output be the empty string.
        let mut output = String::new();

        // 3. For each segment of url's path: append U+002F (/) followed by segment to output.
        for segment in &self.path {
            output.push('/');
            output.push_str(segment);
        }

        // 4. Return output.
        output
    }

    // https://url.spec.whatwg.org/#concept-url-serializer
    pub(crate) fn serialize(&self, exclude_fragment: ExcludeFragment) -> String {
        // 1. Let output be url's scheme and U+003A (:) concatenated.
        let mut output = String::new();
        output.push_str(&self.scheme);
        output.push(':');

        // 2. If url's host is non-null:
        if let Some(host) = self.host.as_ref() {
            // 1. Append "//" to output.
            output.push_str("//");

            // 2. If url includes credentials, then:
            if url_includes_credentials(self) {
                // 1. Append url's username to output.
                output.push_str(&self.username);

                // 2. If url's password is not the empty string, then append U+003A (:), followed by url's password, to output.
                if !self.password.is_empty() {
                    output.push(':');
                    output.push_str(&self.password);
                }

                // 3. Append U+0040 (@) to output.
                output.push('@');
            }

            // 3. Append url's host, serialized, to output.
            output.push_str(&host.serialize());

            // 4. If url's port is non-null, append U+003A (:) followed by url's port, serialized, to output.
            if let Some(port) = self.port {
                output.push(':');
                output.push_str(&port.to_string());
            }
        }

        // 3. If url's host is null, url does not have an opaque path, url's path's size is greater than 1, and url's
        //    path[0] is the empty string, then append U+002F (/) followed by U+002E (.) to output.
        if self.host.is_none() && !self.has_opaque_path && self.path.len() > 1 && self.path[0].is_empty() {
            output.push_str("/.");
        }

        // 4. Append the result of URL path serializing url to output.
        output.push_str(&self.serialize_path());

        // 5. If url's query is non-null, append U+003F (?), followed by url's query, to output.
        if let Some(query) = &self.query {
            output.push('?');
            output.push_str(query);
        }

        // 6. If exclude fragment is false and url's fragment is non-null, then append U+0023 (#), followed by url's
        //    fragment, to output.
        if exclude_fragment == ExcludeFragment::No
            && let Some(fragment) = &self.fragment
        {
            output.push('#');
            output.push_str(fragment);
        }

        // 7. Return output.
        output
    }
}
