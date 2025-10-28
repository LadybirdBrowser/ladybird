/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>

namespace IPC {

// Multibase encoding standards from https://github.com/multiformats/multibase
// Each encoding has a single-character prefix

enum class MultibaseEncoding {
    Base16Lower,     // 'f' - Hexadecimal lowercase
    Base16Upper,     // 'F' - Hexadecimal uppercase
    Base32Lower,     // 'b' - RFC4648 base32 lowercase, no padding
    Base32Upper,     // 'B' - RFC4648 base32 uppercase, no padding
    Base32HexLower,  // 'v' - RFC4648 base32hex lowercase, no padding
    Base32HexUpper,  // 'V' - RFC4648 base32hex uppercase, no padding
    Base32PadLower,  // 'c' - RFC4648 base32 lowercase, with padding
    Base32PadUpper,  // 'C' - RFC4648 base32 uppercase, with padding
    Base32Z,         // 'h' - z-base-32 (used by Tahoe-LAFS)
    Base36Lower,     // 'k' - Base36 lowercase
    Base36Upper,     // 'K' - Base36 uppercase
    Base58BTC,       // 'z' - Bitcoin base58
    Base58Flickr,    // 'Z' - Flickr base58
    Base64,          // 'm' - RFC4648 base64, with padding
    Base64Pad,       // 'M' - RFC4648 base64, with padding
    Base64Url,       // 'u' - RFC4648 base64url, no padding
    Base64UrlPad,    // 'U' - RFC4648 base64url, with padding
    Unknown
};

class Multibase {
public:
    // Detect multibase encoding from prefix character
    static ErrorOr<MultibaseEncoding> detect_encoding(char prefix);

    // Decode multibase-encoded string (with prefix)
    static ErrorOr<ByteBuffer> decode(ByteString const& encoded);

    // Decode without prefix (must specify encoding)
    static ErrorOr<ByteBuffer> decode_raw(ByteString const& encoded, MultibaseEncoding encoding);

    // Encode data with multibase prefix
    static ErrorOr<ByteString> encode(ReadonlyBytes data, MultibaseEncoding encoding);

private:
    // Base-specific decoders
    static ErrorOr<ByteBuffer> decode_base16(ByteString const& encoded, bool uppercase);
    static ErrorOr<ByteBuffer> decode_base32(ByteString const& encoded, bool uppercase, bool padded);
    static ErrorOr<ByteBuffer> decode_base58(ByteString const& encoded, bool flickr_alphabet);
    static ErrorOr<ByteBuffer> decode_base64(ByteString const& encoded, bool url_safe, bool padded);

    // Base-specific encoders
    static ErrorOr<ByteString> encode_base16(ReadonlyBytes data, bool uppercase);
    static ErrorOr<ByteString> encode_base32(ReadonlyBytes data, bool uppercase, bool padded);
    static ErrorOr<ByteString> encode_base58(ReadonlyBytes data, bool flickr_alphabet);
    static ErrorOr<ByteString> encode_base64(ReadonlyBytes data, bool url_safe, bool padded);
};

}
