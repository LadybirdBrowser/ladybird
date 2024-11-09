/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibCrypto/Curves/EllipticCurve.h>

namespace Crypto::Curves {

struct Ed25519Point {
    u32 x[8] {};
    u32 y[8] {};
    u32 z[8] {};
    u32 t[8] {};
};

class Ed25519 {
public:
    static constexpr Ed25519Point BASE_POINT = {
        { 0x8F25D51A, 0xC9562D60, 0x9525A7B2, 0x692CC760, 0xFDD6DC5C, 0xC0A4E231, 0xCD6E53FE, 0x216936D3 },
        { 0x66666658, 0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666666 },
        { 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
        { 0xA5B7DDA3, 0x6DDE8AB3, 0x775152F5, 0x20F09F80, 0x64ABE37D, 0x66EA4E8E, 0xD78B7665, 0x67875F0F }
    };

    size_t key_size() { return 32; }
    size_t signature_size() { return 64; }
    ErrorOr<ByteBuffer> generate_private_key();
    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes private_key);

    ErrorOr<ByteBuffer> sign(ReadonlyBytes public_key, ReadonlyBytes private_key, ReadonlyBytes message);
    bool verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message);

private:
    void encode_point(Ed25519Point* point, u8* data);
    u32 decode_point(Ed25519Point* point, u8 const* data);

    void point_add(Ed25519Point* result, Ed25519Point const* p, Ed25519Point const* q);
    void point_double(Ed25519Point* result, Ed25519Point const* point);
    void point_multiply_scalar(Ed25519Point* result, u8 const* scalar, Ed25519Point const* point);

    void barrett_reduce(u8* result, u8 const* input);

    void add(u8* result, u8 const* a, u8 const* b, u8 n);
    u8 subtract(u8* result, u8 const* a, u8 const* b, u8 n);
    void multiply(u8* result_low, u8* result_high, u8 const* a, u8 const* b, u8 n);

    void select(u8* result, u8 const* a, u8 const* b, u8 c, u8 n);
    u8 compare(u8 const* a, u8 const* b, u8 n);
    void copy(u8* a, u8 const* b, u32 n);

    u8 k[64] {};
    u8 p[32] {};
    u8 r[32] {};
    u8 s[32] {};
    Ed25519Point ka {};
    Ed25519Point rb {};
    Ed25519Point sb {};
    Ed25519Point u {};
    Ed25519Point v {};
    u32 a[8] {};
    u32 b[8] {};
    u32 c[8] {};
    u32 d[8] {};
    u32 e[8] {};
    u32 f[8] {};
    u32 g[8] {};
    u32 h[8] {};
};

}
