/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Hex.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibIPC/IPFSVerifier.h>
#include <LibIPC/Multibase.h>
#include <LibIPC/Multicodec.h>
#include <LibIPC/Multihash.h>

namespace IPC {

ErrorOr<CIDVersion> IPFSVerifier::detect_version(ByteString const& cid_string)
{
    // CIDv0: Starts with "Qm" and is exactly 46 characters (Base58 SHA-256)
    if (cid_string.starts_with("Qm"sv) && cid_string.length() == 46)
        return CIDVersion::V0;

    // CIDv1: Starts with multibase prefix (commonly "bafy", "bafk", "bafz", etc.)
    if (cid_string.starts_with("baf"sv))
        return CIDVersion::V1;

    return Error::from_string_literal("Unknown CID format - must start with 'Qm' (v0) or 'baf' (v1)");
}

ErrorOr<ByteBuffer> IPFSVerifier::decode_base58(ByteString const& input)
{
    // Base58 alphabet (Bitcoin/IPFS variant)
    static constexpr char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    // Build character to value map
    u8 char_map[256];
    for (size_t i = 0; i < 256; i++)
        char_map[i] = 255;
    for (size_t i = 0; i < 58; i++)
        char_map[static_cast<u8>(alphabet[i])] = i;

    // Decode Base58 to bytes
    Vector<u8> result;
    result.resize(input.length() * 2); // Overallocate
    size_t result_len = 0;

    for (size_t i = 0; i < input.length(); i++) {
        u8 c = input[i];
        if (char_map[c] == 255)
            return Error::from_string_literal("Invalid Base58 character");

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
    for (size_t i = 0; i < input.length() && input[i] == '1'; i++)
        leading_zeros++;

    // Reverse bytes (Base58 is big-endian)
    ByteBuffer decoded = TRY(ByteBuffer::create_uninitialized(leading_zeros + result_len));
    for (size_t i = 0; i < leading_zeros; i++)
        decoded[i] = 0;
    for (size_t i = 0; i < result_len; i++)
        decoded[leading_zeros + i] = result[result_len - 1 - i];

    return decoded;
}

ErrorOr<ParsedCID> IPFSVerifier::parse_cid_v0(ByteString const& cid_string)
{
    // CIDv0 format: Base58-encoded multihash
    // Multihash format: <hash-type><hash-length><hash-bytes>
    // For SHA-256: 0x12 (SHA-256) 0x20 (32 bytes) <32 hash bytes>

    auto decoded = TRY(decode_base58(cid_string));

    // Validate multihash structure
    if (decoded.size() < 2)
        return Error::from_string_literal("CIDv0 multihash too short");

    u8 hash_type = decoded[0];
    u8 hash_length = decoded[1];

    // CIDv0 always uses SHA-256 (0x12)
    if (hash_type != 0x12)
        return Error::from_string_literal("CIDv0 must use SHA-256 (hash type 0x12)");

    // SHA-256 produces 32 bytes
    if (hash_length != 32)
        return Error::from_string_literal("CIDv0 SHA-256 must be 32 bytes");

    if (decoded.size() != 34) // 2 (header) + 32 (hash)
        return Error::from_string_literal("CIDv0 multihash incorrect size");

    // Extract hash bytes (skip 2-byte header)
    ByteBuffer expected_hash = TRY(ByteBuffer::copy(decoded.bytes().slice(2)));

    return ParsedCID {
        .version = CIDVersion::V0,
        .raw_cid = cid_string,
        .expected_hash = move(expected_hash),
        .hash_algorithm = "sha256"sv
    };
}

ErrorOr<ParsedCID> IPFSVerifier::parse_cid_v1(ByteString const& cid_string)
{
    // CIDv1 format: <multibase-prefix><version><codec><multihash>
    // Full implementation using multibase/multicodec/multihash libraries

    // Step 1: Multibase decode (removes prefix and decodes)
    auto decoded = TRY(Multibase::decode(cid_string));

    if (decoded.size() < 2)
        return Error::from_string_literal("CIDv1 decoded data too short");

    // Step 2: Extract version byte (should be 0x01 for CIDv1)
    u8 version = decoded[0];
    if (version != 0x01)
        return Error::from_string_literal("CIDv1 version byte must be 0x01");

    // Step 3: Decode codec (varint after version)
    size_t codec_bytes_read = 0;
    auto remaining_data = decoded.bytes().slice(1);
    u64 codec_code = TRY(Multihash::decode_varint(remaining_data, codec_bytes_read));

    dbgln("IPFS: CIDv1 codec = {} ({})", Multicodec::codec_name(codec_code), codec_code);

    // Step 4: Parse multihash (remaining data after version + codec)
    auto multihash_data = remaining_data.slice(codec_bytes_read);
    auto parsed_multihash = TRY(Multihash::parse_with_varint(multihash_data));

    dbgln("IPFS: CIDv1 multihash algorithm = {}, length = {}",
        parsed_multihash.hash_algorithm_name(),
        parsed_multihash.hash_length);

    // Step 5: Build ParsedCID with extracted hash
    return ParsedCID {
        .version = CIDVersion::V1,
        .raw_cid = cid_string,
        .expected_hash = move(parsed_multihash.hash_bytes),
        .hash_algorithm = parsed_multihash.hash_algorithm_name()
    };
}

ErrorOr<ParsedCID> IPFSVerifier::parse_cid(ByteString const& cid_string)
{
    auto version = TRY(detect_version(cid_string));

    switch (version) {
    case CIDVersion::V0:
        return parse_cid_v0(cid_string);
    case CIDVersion::V1:
        return parse_cid_v1(cid_string);
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<ByteBuffer> IPFSVerifier::hash_content(ReadonlyBytes content, ByteString const& algorithm)
{
    if (algorithm == "sha256"sv) {
        auto digest = Crypto::Hash::SHA256::hash(content);
        return ByteBuffer::copy(digest.bytes());
    }

    return Error::from_string_literal("Unsupported hash algorithm - only SHA-256 supported currently");
}

ErrorOr<bool> IPFSVerifier::verify_content(ParsedCID const& cid, ReadonlyBytes content)
{
    // Hash the content using the algorithm specified in the CID
    auto computed_hash = TRY(hash_content(content, cid.hash_algorithm));

    // Compare hashes
    if (computed_hash.size() != cid.expected_hash.size()) {
        dbgln("IPFSVerifier: Hash size mismatch - expected {}, got {}",
            cid.expected_hash.size(), computed_hash.size());
        return false;
    }

    bool matches = (computed_hash.bytes() == cid.expected_hash.bytes());

    if (matches) {
        dbgln("IPFSVerifier: Content integrity verified for {} {}",
            cid.version == CIDVersion::V0 ? "CIDv0" : "CIDv1",
            cid.raw_cid);
    } else {
        dbgln("IPFSVerifier: HASH MISMATCH for {} {}",
            cid.version == CIDVersion::V0 ? "CIDv0" : "CIDv1",
            cid.raw_cid);
        dbgln("  Expected: {}", encode_hex(cid.expected_hash));
        dbgln("  Computed: {}", encode_hex(computed_hash));
    }

    return matches;
}

}
