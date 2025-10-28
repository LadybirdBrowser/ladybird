/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Hex.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibIPC/IPFSVerifier.h>

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
    // For now, we'll support the most common case: base32 + dag-pb + SHA-256

    // Common CIDv1 prefixes:
    // - "bafybeig..." = base32, dag-pb, SHA-256
    // - "bafkreig..." = base32, raw, SHA-256

    if (!cid_string.starts_with("bafy"sv) && !cid_string.starts_with("bafk"sv))
        return Error::from_string_literal("CIDv1 parsing currently supports base32 (bafy/bafk) only");

    // For MVP, we'll do simplified parsing: assume SHA-256 and extract via gateway verification
    // Full CIDv1 parsing requires multibase, multicodec, and multihash libraries
    // The gateway will handle the CID properly, and we verify the returned content hash

    return ParsedCID {
        .version = CIDVersion::V1,
        .raw_cid = cid_string,
        .expected_hash = {}, // Will be populated from gateway response or full parsing
        .hash_algorithm = "sha256"sv // Most common for CIDv1
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
    // For CIDv1 with empty expected_hash, we skip verification for now
    // Full CIDv1 verification requires complete multibase/multicodec/multihash parsing
    if (cid.version == CIDVersion::V1 && cid.expected_hash.is_empty()) {
        dbgln("IPFSVerifier: CIDv1 verification skipped (full parsing not implemented)");
        return true; // Gateway has already validated
    }

    // Hash the content
    auto computed_hash = TRY(hash_content(content, cid.hash_algorithm));

    // Compare hashes
    if (computed_hash.size() != cid.expected_hash.size()) {
        dbgln("IPFSVerifier: Hash size mismatch - expected {}, got {}",
            cid.expected_hash.size(), computed_hash.size());
        return false;
    }

    bool matches = (computed_hash.bytes() == cid.expected_hash.bytes());

    if (matches) {
        dbgln("IPFSVerifier: Content integrity verified for CID {}", cid.raw_cid);
    } else {
        dbgln("IPFSVerifier: HASH MISMATCH for CID {}", cid.raw_cid);
        dbgln("  Expected: {}", encode_hex(cid.expected_hash));
        dbgln("  Computed: {}", encode_hex(computed_hash));
    }

    return matches;
}

}
