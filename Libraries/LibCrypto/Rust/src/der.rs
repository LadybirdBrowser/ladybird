/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The few DER structures that Web Crypto exports or checks. Each one is written into a single buffer of its final size,
//! so that no partial copy of a private key is left behind, which OpenSSL's encoders do leave.
//! Object identifiers are given as the contents of their DER encoding.

pub(crate) const INTEGER: u8 = 0x02;
pub(crate) const BIT_STRING: u8 = 0x03;
pub(crate) const OCTET_STRING: u8 = 0x04;
pub(crate) const OBJECT_IDENTIFIER: u8 = 0x06;
pub(crate) const SEQUENCE: u8 = 0x30;
pub(crate) const NULL: [u8; 2] = [0x05, 0x00];

/// The tag and definite length that start an element of `length` content bytes.
pub(crate) fn header(tag: u8, length: usize) -> Vec<u8> {
    let mut header = vec![tag];
    if length < 0x80 {
        header.push(length as u8);
    } else {
        let bytes = length.to_be_bytes();
        let leading_zeros = bytes.iter().take_while(|&&byte| byte == 0).count();
        header.push(0x80 | (bytes.len() - leading_zeros) as u8);
        header.extend_from_slice(&bytes[leading_zeros..]);
    }
    header
}

/// An element of the given tag and contents, the contents given in parts.
pub(crate) fn element(tag: u8, contents: &[&[u8]]) -> Vec<u8> {
    let length = contents.iter().map(|part| part.len()).sum();
    let header = header(tag, length);
    let mut output = Vec::with_capacity(header.len() + length);
    output.extend_from_slice(&header);
    for part in contents {
        output.extend_from_slice(part);
    }
    output
}

/// The object identifier of the given DER contents.
pub(crate) fn object_identifier(contents: &[u8]) -> Vec<u8> {
    element(OBJECT_IDENTIFIER, &[contents])
}

fn algorithm_identifier(algorithm: &[u8], parameters: Option<&[u8]>) -> Vec<u8> {
    element(
        SEQUENCE,
        &[&object_identifier(algorithm), parameters.unwrap_or_default()],
    )
}

/// https://www.rfc-editor.org/rfc/rfc8017#appendix-A.1.2
pub(crate) fn rsa_private_key(integers: &[&[u8]]) -> Vec<u8> {
    // RSAPrivateKey ::= SEQUENCE {
    //     version           Version,
    //     modulus           INTEGER,  -- n
    //     publicExponent    INTEGER,  -- e
    //     privateExponent   INTEGER,  -- d
    //     prime1            INTEGER,  -- p
    //     prime2            INTEGER,  -- q
    //     exponent1         INTEGER,  -- d mod (p-1)
    //     exponent2         INTEGER,  -- d mod (q-1)
    //     coefficient       INTEGER,  -- (inverse of q) mod p
    //     otherPrimeInfos   OtherPrimeInfos OPTIONAL
    // }
    // NB: The integers are unsigned and have no leading zeros, so a zero byte keeps one whose top bit is set positive.
    let version = [INTEGER, 1, 0];
    let prefixes: Vec<Vec<u8>> = integers
        .iter()
        .map(|integer| {
            let needs_zero = integer.first().is_none_or(|&byte| byte & 0x80 != 0);
            let mut prefix = header(INTEGER, integer.len() + usize::from(needs_zero));
            if needs_zero {
                prefix.push(0);
            }
            prefix
        })
        .collect();
    let mut contents: Vec<&[u8]> = vec![&version];
    for (prefix, integer) in prefixes.iter().zip(integers) {
        contents.push(prefix);
        contents.push(integer);
    }
    element(SEQUENCE, &contents)
}

/// https://www.rfc-editor.org/rfc/rfc5280#section-4.1
pub(crate) fn subject_public_key_info(algorithm: &[u8], parameters: Option<&[u8]>, public_key: &[u8]) -> Vec<u8> {
    let bit_string = element(BIT_STRING, &[&[0], public_key]);
    element(SEQUENCE, &[&algorithm_identifier(algorithm, parameters), &bit_string])
}

/// https://www.rfc-editor.org/rfc/rfc5208#section-5
pub(crate) fn private_key_info(algorithm: &[u8], parameters: Option<&[u8]>, private_key: &[u8]) -> Vec<u8> {
    let version = [INTEGER, 1, 0];
    let private_key_header = header(OCTET_STRING, private_key.len());
    element(
        SEQUENCE,
        &[
            &version,
            &algorithm_identifier(algorithm, parameters),
            &private_key_header,
            private_key,
        ],
    )
}
