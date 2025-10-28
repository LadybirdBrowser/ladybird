/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Hex.h>
#include <AK/Vector.h>
#include <LibIPC/Multibase.h>

namespace IPC {

ErrorOr<MultibaseEncoding> Multibase::detect_encoding(char prefix)
{
    switch (prefix) {
    case 'f':
        return MultibaseEncoding::Base16Lower;
    case 'F':
        return MultibaseEncoding::Base16Upper;
    case 'b':
        return MultibaseEncoding::Base32Lower;
    case 'B':
        return MultibaseEncoding::Base32Upper;
    case 'c':
        return MultibaseEncoding::Base32PadLower;
    case 'C':
        return MultibaseEncoding::Base32PadUpper;
    case 'v':
        return MultibaseEncoding::Base32HexLower;
    case 'V':
        return MultibaseEncoding::Base32HexUpper;
    case 'h':
        return MultibaseEncoding::Base32Z;
    case 'k':
        return MultibaseEncoding::Base36Lower;
    case 'K':
        return MultibaseEncoding::Base36Upper;
    case 'z':
        return MultibaseEncoding::Base58BTC;
    case 'Z':
        return MultibaseEncoding::Base58Flickr;
    case 'm':
        return MultibaseEncoding::Base64;
    case 'M':
        return MultibaseEncoding::Base64Pad;
    case 'u':
        return MultibaseEncoding::Base64Url;
    case 'U':
        return MultibaseEncoding::Base64UrlPad;
    default:
        return Error::from_string_literal("Unknown multibase encoding prefix");
    }
}

ErrorOr<ByteBuffer> Multibase::decode(ByteString const& encoded)
{
    if (encoded.is_empty())
        return Error::from_string_literal("Empty multibase string");

    // First character is the encoding prefix
    char prefix = encoded[0];
    auto encoding = TRY(detect_encoding(prefix));

    // Decode the rest (without prefix)
    auto data_to_decode = encoded.substring(1);
    return decode_raw(data_to_decode, encoding);
}

ErrorOr<ByteBuffer> Multibase::decode_raw(ByteString const& encoded, MultibaseEncoding encoding)
{
    switch (encoding) {
    case MultibaseEncoding::Base16Lower:
        return decode_base16(encoded, false);
    case MultibaseEncoding::Base16Upper:
        return decode_base16(encoded, true);
    case MultibaseEncoding::Base32Lower:
    case MultibaseEncoding::Base32Upper:
        return decode_base32(encoded, encoding == MultibaseEncoding::Base32Upper, false);
    case MultibaseEncoding::Base32PadLower:
    case MultibaseEncoding::Base32PadUpper:
        return decode_base32(encoded, encoding == MultibaseEncoding::Base32PadUpper, true);
    case MultibaseEncoding::Base58BTC:
        return decode_base58(encoded, false);
    case MultibaseEncoding::Base58Flickr:
        return decode_base58(encoded, true);
    case MultibaseEncoding::Base64:
    case MultibaseEncoding::Base64Pad:
        return decode_base64(encoded, false, true);
    case MultibaseEncoding::Base64Url:
        return decode_base64(encoded, true, false);
    case MultibaseEncoding::Base64UrlPad:
        return decode_base64(encoded, true, true);
    default:
        return Error::from_string_literal("Unsupported multibase encoding");
    }
}

ErrorOr<ByteBuffer> Multibase::decode_base16(ByteString const& encoded, bool uppercase)
{
    // Hex decoding (straightforward)
    return decode_hex(encoded);
}

ErrorOr<ByteBuffer> Multibase::decode_base32(ByteString const& encoded, bool uppercase, bool padded)
{
    // RFC 4648 Base32 alphabet
    static constexpr char const* alphabet_lower = "abcdefghijklmnopqrstuvwxyz234567";
    static constexpr char const* alphabet_upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    auto const* alphabet = uppercase ? alphabet_upper : alphabet_lower;

    // Build reverse lookup table
    u8 char_map[256];
    for (size_t i = 0; i < 256; i++)
        char_map[i] = 255;
    for (size_t i = 0; i < 32; i++) {
        char_map[static_cast<u8>(alphabet[i])] = i;
        // Support both cases during decoding
        char_map[static_cast<u8>(tolower(alphabet[i]))] = i;
        char_map[static_cast<u8>(toupper(alphabet[i]))] = i;
    }

    // Remove padding if present
    auto input = encoded;
    if (padded) {
        // Remove '=' padding
        while (input.ends_with("="sv))
            input = input.substring(0, input.length() - 1);
    }

    // Calculate output size (5 bits per character → 8 bits per byte)
    size_t bit_count = input.length() * 5;
    size_t byte_count = bit_count / 8;

    auto result = TRY(ByteBuffer::create_uninitialized(byte_count));
    size_t result_index = 0;

    u32 buffer = 0;
    int bits_in_buffer = 0;

    for (size_t i = 0; i < input.length(); i++) {
        u8 c = input[i];
        if (char_map[c] == 255)
            return Error::from_string_literal("Invalid base32 character");

        u8 value = char_map[c];
        buffer = (buffer << 5) | value;
        bits_in_buffer += 5;

        if (bits_in_buffer >= 8) {
            bits_in_buffer -= 8;
            result[result_index++] = (buffer >> bits_in_buffer) & 0xFF;
        }
    }

    return result;
}

ErrorOr<ByteBuffer> Multibase::decode_base58(ByteString const& encoded, bool flickr_alphabet)
{
    // Base58 alphabet (Bitcoin-style or Flickr-style)
    static constexpr char const* alphabet_btc = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    static constexpr char const* alphabet_flickr = "123456789abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ";

    auto const* alphabet = flickr_alphabet ? alphabet_flickr : alphabet_btc;

    // Build character to value map
    u8 char_map[256];
    for (size_t i = 0; i < 256; i++)
        char_map[i] = 255;
    for (size_t i = 0; i < 58; i++)
        char_map[static_cast<u8>(alphabet[i])] = i;

    // Decode Base58 to bytes
    Vector<u8> result;
    result.resize(encoded.length() * 2); // Overallocate
    size_t result_len = 0;

    for (size_t i = 0; i < encoded.length(); i++) {
        u8 c = encoded[i];
        if (char_map[c] == 255)
            return Error::from_string_literal("Invalid base58 character");

        u32 carry = char_map[c];
        for (size_t j = 0; j < result_len; j++) {
            carry += static_cast<u32>(result[j]) * 58;
            result[j] = carry & 0xFF;
            carry >>= 8;
        }

        while (carry > 0) {
            result[result_len++] = carry & 0xFF;
            carry >>= 8;
        }
    }

    // Count leading zeros in input (preserved as leading zeros in output)
    size_t leading_zeros = 0;
    for (size_t i = 0; i < encoded.length() && encoded[i] == alphabet[0]; i++)
        leading_zeros++;

    // Reverse bytes (Base58 is big-endian)
    ByteBuffer decoded = TRY(ByteBuffer::create_uninitialized(leading_zeros + result_len));
    for (size_t i = 0; i < leading_zeros; i++)
        decoded[i] = 0;
    for (size_t i = 0; i < result_len; i++)
        decoded[leading_zeros + i] = result[result_len - 1 - i];

    return decoded;
}

ErrorOr<ByteBuffer> Multibase::decode_base64(ByteString const& encoded, bool url_safe, bool padded)
{
    // Use AK::Base64 for decoding
    // Note: AK::Base64 handles standard base64 with padding

    if (url_safe) {
        // Convert URL-safe characters back to standard base64
        auto standard_encoded = encoded;
        standard_encoded = standard_encoded.replace("-"sv, "+"sv, ReplaceMode::All);
        standard_encoded = standard_encoded.replace("_"sv, "/"sv, ReplaceMode::All);

        // Add padding if needed and not present
        if (padded) {
            while (standard_encoded.length() % 4 != 0)
                standard_encoded = ByteString::formatted("{}=", standard_encoded);
        }

        return decode_base64(standard_encoded);
    }

    // Standard base64 decoding
    return decode_base64(encoded);
}

ErrorOr<ByteString> Multibase::encode(ReadonlyBytes data, MultibaseEncoding encoding)
{
    // Get the prefix character
    char prefix;
    switch (encoding) {
    case MultibaseEncoding::Base16Lower:
        prefix = 'f';
        break;
    case MultibaseEncoding::Base16Upper:
        prefix = 'F';
        break;
    case MultibaseEncoding::Base32Lower:
        prefix = 'b';
        break;
    case MultibaseEncoding::Base32Upper:
        prefix = 'B';
        break;
    case MultibaseEncoding::Base58BTC:
        prefix = 'z';
        break;
    case MultibaseEncoding::Base64:
        prefix = 'm';
        break;
    default:
        return Error::from_string_literal("Unsupported multibase encoding for encode");
    }

    // Encode the data (encoding functions to be implemented as needed)
    return Error::from_string_literal("Multibase encoding not yet implemented");
}

}
