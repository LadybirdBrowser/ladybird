/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Multihash.h>

namespace IPC {

ByteString ParsedMultihash::hash_algorithm_name() const
{
    return Multihash::hash_algorithm_name(hash_code);
}

ErrorOr<u64> Multihash::decode_varint(ReadonlyBytes data, size_t& bytes_read)
{
    u64 value = 0;
    size_t shift = 0;
    bytes_read = 0;

    for (size_t i = 0; i < data.size() && i < 9; i++) { // Max 9 bytes for u64
        u8 byte = data[i];
        bytes_read++;

        value |= static_cast<u64>(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0) {
            // MSB is 0, this is the last byte
            return value;
        }

        shift += 7;
    }

    return Error::from_string_literal("Varint decoding failed - too many bytes or incomplete");
}

ErrorOr<ParsedMultihash> Multihash::parse(ReadonlyBytes data)
{
    if (data.size() < 2)
        return Error::from_string_literal("Multihash too short - need at least 2 bytes");

    // Simple parsing: first byte is hash code, second byte is hash length
    u8 hash_code_byte = data[0];
    u8 hash_length = data[1];

    // Verify we have enough data
    if (data.size() < static_cast<size_t>(2 + hash_length))
        return Error::from_string_literal("Multihash data truncated");

    // Extract hash bytes
    auto hash_bytes = TRY(ByteBuffer::copy(data.slice(2, hash_length)));

    return ParsedMultihash {
        .hash_code = static_cast<MultihashCode>(hash_code_byte),
        .hash_length = hash_length,
        .hash_bytes = move(hash_bytes)
    };
}

ErrorOr<ParsedMultihash> Multihash::parse_with_varint(ReadonlyBytes data)
{
    if (data.is_empty())
        return Error::from_string_literal("Multihash data is empty");

    // Decode hash code (varint)
    size_t hash_code_bytes = 0;
    u64 hash_code = TRY(decode_varint(data, hash_code_bytes));

    if (hash_code_bytes >= data.size())
        return Error::from_string_literal("Multihash truncated after hash code");

    // Decode hash length (varint)
    size_t hash_length_bytes = 0;
    u64 hash_length_u64 = TRY(decode_varint(data.slice(hash_code_bytes), hash_length_bytes));

    if (hash_length_u64 > 255)
        return Error::from_string_literal("Multihash length too large");

    u8 hash_length = static_cast<u8>(hash_length_u64);

    size_t hash_start = hash_code_bytes + hash_length_bytes;
    if (hash_start + hash_length > data.size())
        return Error::from_string_literal("Multihash data truncated");

    // Extract hash bytes
    auto hash_bytes = TRY(ByteBuffer::copy(data.slice(hash_start, hash_length)));

    return ParsedMultihash {
        .hash_code = static_cast<MultihashCode>(hash_code),
        .hash_length = hash_length,
        .hash_bytes = move(hash_bytes)
    };
}

ByteString Multihash::hash_algorithm_name(MultihashCode code)
{
    switch (code) {
    case MultihashCode::Identity:
        return "identity"sv;
    case MultihashCode::SHA1:
        return "sha1"sv;
    case MultihashCode::SHA2_256:
        return "sha256"sv;
    case MultihashCode::SHA2_512:
        return "sha512"sv;
    case MultihashCode::SHA3_512:
        return "sha3-512"sv;
    case MultihashCode::SHA3_384:
        return "sha3-384"sv;
    case MultihashCode::SHA3_256:
        return "sha3-256"sv;
    case MultihashCode::SHA3_224:
        return "sha3-224"sv;
    case MultihashCode::Blake2b_256:
        return "blake2b-256"sv;
    case MultihashCode::Blake2b_512:
        return "blake2b-512"sv;
    case MultihashCode::Blake2s_128:
        return "blake2s-128"sv;
    case MultihashCode::Blake2s_256:
        return "blake2s-256"sv;
    default:
        return ByteString::formatted("unknown-{}", static_cast<u64>(code));
    }
}

ErrorOr<u8> Multihash::expected_hash_length(MultihashCode code)
{
    switch (code) {
    case MultihashCode::Identity:
        return 0; // Variable length
    case MultihashCode::SHA1:
        return 20;
    case MultihashCode::SHA2_256:
        return 32;
    case MultihashCode::SHA2_512:
        return 64;
    case MultihashCode::SHA3_512:
        return 64;
    case MultihashCode::SHA3_384:
        return 48;
    case MultihashCode::SHA3_256:
        return 32;
    case MultihashCode::SHA3_224:
        return 28;
    case MultihashCode::Blake2b_256:
        return 32;
    case MultihashCode::Blake2b_512:
        return 64;
    case MultihashCode::Blake2s_128:
        return 16;
    case MultihashCode::Blake2s_256:
        return 32;
    default:
        return Error::from_string_literal("Unknown hash algorithm");
    }
}

ErrorOr<ByteBuffer> Multihash::create(MultihashCode code, ReadonlyBytes hash_bytes)
{
    u8 hash_code_byte = static_cast<u8>(code);
    u8 hash_length = hash_bytes.size();

    // Simple encoding: <hash-code><hash-length><hash-bytes>
    auto multihash = TRY(ByteBuffer::create_uninitialized(2 + hash_length));
    multihash[0] = hash_code_byte;
    multihash[1] = hash_length;
    memcpy(multihash.data() + 2, hash_bytes.data(), hash_bytes.size());

    return multihash;
}

}
