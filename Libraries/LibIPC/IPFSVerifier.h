/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>

namespace IPC {

// IPFS CID (Content Identifier) formats:
// - CIDv0: Base58-encoded SHA-256 multihash starting with "Qm" (46 chars)
// - CIDv1: Multibase-encoded with version/codec/hash (starts with "bafy", "bafk", etc.)
enum class CIDVersion {
    V0, // Base58, SHA-256, 46 characters, starts with "Qm"
    V1  // Multibase, various codecs and hashes
};

struct ParsedCID {
    CIDVersion version;
    ByteString raw_cid;           // Original CID string
    ByteBuffer expected_hash;     // Decoded hash bytes for comparison
    ByteString hash_algorithm;    // "sha256", "blake2b-256", etc.
};

class IPFSVerifier {
public:
    // Parse CID from ipfs:// URL path
    static ErrorOr<ParsedCID> parse_cid(ByteString const& cid_string);

    // Verify fetched content matches CID hash
    static ErrorOr<bool> verify_content(ParsedCID const& cid, ReadonlyBytes content);

    // Hash content using specified algorithm
    static ErrorOr<ByteBuffer> hash_content(ReadonlyBytes content, ByteString const& algorithm);

private:
    // CIDv0 parsing (Base58 SHA-256 multihash)
    static ErrorOr<ParsedCID> parse_cid_v0(ByteString const& cid_string);

    // CIDv1 parsing (Multibase encoded)
    static ErrorOr<ParsedCID> parse_cid_v1(ByteString const& cid_string);

    // Base58 decoding for CIDv0
    static ErrorOr<ByteBuffer> decode_base58(ByteString const& input);

    // Detect CID version from string format
    static ErrorOr<CIDVersion> detect_version(ByteString const& cid_string);
};

}
