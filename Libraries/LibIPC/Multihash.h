/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/StringView.h>

namespace IPC {

// Multihash hash codes from https://github.com/multiformats/multicodec
enum class MultihashCode : u64 {
    Identity = 0x00,      // Identity hash (no hashing)
    SHA1 = 0x11,          // SHA-1 (deprecated, 20 bytes)
    SHA2_256 = 0x12,      // SHA-256 (32 bytes)
    SHA2_512 = 0x13,      // SHA-512 (64 bytes)
    SHA3_512 = 0x14,      // SHA3-512
    SHA3_384 = 0x15,      // SHA3-384
    SHA3_256 = 0x16,      // SHA3-256
    SHA3_224 = 0x17,      // SHA3-224
    Blake2b_256 = 0x1b,   // Blake2b-256
    Blake2b_512 = 0x1c,   // Blake2b-512
    Blake2s_128 = 0x1d,   // Blake2s-128
    Blake2s_256 = 0x1e,   // Blake2s-256
};

struct ParsedMultihash {
    MultihashCode hash_code;
    u8 hash_length;
    ByteBuffer hash_bytes;

    ByteString hash_algorithm_name() const;
};

class Multihash {
public:
    // Parse multihash from bytes (format: <hash-code><hash-length><hash-bytes>)
    static ErrorOr<ParsedMultihash> parse(ReadonlyBytes data);

    // Parse multihash with varint support (for codes > 127)
    static ErrorOr<ParsedMultihash> parse_with_varint(ReadonlyBytes data);

    // Get hash algorithm name from code
    static ByteString hash_algorithm_name(MultihashCode code);

    // Get expected hash length for a given algorithm
    static ErrorOr<u8> expected_hash_length(MultihashCode code);

    // Create multihash from hash code and hash bytes
    static ErrorOr<ByteBuffer> create(MultihashCode code, ReadonlyBytes hash_bytes);

private:
    // Decode unsigned varint (variable-length integer encoding)
    static ErrorOr<u64> decode_varint(ReadonlyBytes data, size_t& bytes_read);
};

}
