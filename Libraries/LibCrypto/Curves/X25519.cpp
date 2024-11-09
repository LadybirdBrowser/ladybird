/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Endian.h>
#include <AK/Random.h>
#include <LibCrypto/Curves/Curve25519.h>
#include <LibCrypto/Curves/X25519.h>

namespace Crypto::Curves {

static constexpr u8 BITS = 255;
static constexpr u8 BYTES = 32;
static constexpr u8 WORDS = 8;
static constexpr u32 A24 = 121666;

static void conditional_swap(u32* first, u32* second, u32 condition)
{
    u32 mask = ~condition + 1;
    for (auto i = 0; i < WORDS; i++) {
        u32 temp = mask & (first[i] ^ second[i]);
        first[i] ^= temp;
        second[i] ^= temp;
    }
}

ErrorOr<ByteBuffer> X25519::generate_private_key()
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(BYTES));
    fill_with_random(buffer);
    return buffer;
}

ErrorOr<ByteBuffer> X25519::generate_public_key(ReadonlyBytes a)
{
    u8 generator[BYTES] { 9 };
    return compute_coordinate(a, { generator, BYTES });
}

// https://datatracker.ietf.org/doc/html/rfc7748#section-5
ErrorOr<ByteBuffer> X25519::compute_coordinate(ReadonlyBytes input_k, ReadonlyBytes input_u)
{
    u32 k[WORDS] {};
    u32 u[WORDS] {};
    u32 x1[WORDS] {};
    u32 x2[WORDS] {};
    u32 z1[WORDS] {};
    u32 z2[WORDS] {};
    u32 t1[WORDS] {};
    u32 t2[WORDS] {};

    // Copy input to internal state
    Curve25519::import_state(k, input_k.data());

    // Set the three least significant bits of the first byte and the most significant bit of the last to zero,
    // set the second most significant bit of the last byte to 1
    k[0] &= 0xFFFFFFF8;
    k[7] &= 0x7FFFFFFF;
    k[7] |= 0x40000000;

    // Copy coordinate to internal state
    Curve25519::import_state(u, input_u.data());
    // mask the most significant bit in the final byte.
    u[7] &= 0x7FFFFFFF;

    // Implementations MUST accept non-canonical values and process them as
    // if they had been reduced modulo the field prime.
    Curve25519::modular_reduce(u, u);

    Curve25519::set(x1, 1);
    Curve25519::set(z1, 0);
    Curve25519::copy(x2, u);
    Curve25519::set(z2, 1);

    // Montgomery ladder
    u32 swap = 0;
    for (auto i = BITS - 1; i >= 0; i--) {
        u32 b = (k[i / BYTES] >> (i % BYTES)) & 1;

        conditional_swap(x1, x2, swap ^ b);
        conditional_swap(z1, z2, swap ^ b);

        swap = b;

        Curve25519::modular_add(t1, x2, z2);
        Curve25519::modular_subtract(x2, x2, z2);
        Curve25519::modular_add(z2, x1, z1);
        Curve25519::modular_subtract(x1, x1, z1);
        Curve25519::modular_multiply(t1, t1, x1);
        Curve25519::modular_multiply(x2, x2, z2);
        Curve25519::modular_square(z2, z2);
        Curve25519::modular_square(x1, x1);
        Curve25519::modular_subtract(t2, z2, x1);
        Curve25519::modular_multiply_single(z1, t2, A24);
        Curve25519::modular_add(z1, z1, x1);
        Curve25519::modular_multiply(z1, z1, t2);
        Curve25519::modular_multiply(x1, x1, z2);
        Curve25519::modular_subtract(z2, t1, x2);
        Curve25519::modular_square(z2, z2);
        Curve25519::modular_multiply(z2, z2, u);
        Curve25519::modular_add(x2, x2, t1);
        Curve25519::modular_square(x2, x2);
    }

    conditional_swap(x1, x2, swap);
    conditional_swap(z1, z2, swap);

    // Retrieve affine representation
    Curve25519::modular_multiply_inverse(u, z1);
    Curve25519::modular_multiply(u, u, x1);

    // Encode state for export
    auto buffer = TRY(ByteBuffer::create_uninitialized(BYTES));
    Curve25519::export_state(u, buffer.data());

    return buffer;
}

ErrorOr<ByteBuffer> X25519::derive_premaster_key(ReadonlyBytes shared_point)
{
    VERIFY(shared_point.size() == BYTES);
    ByteBuffer premaster_key = TRY(ByteBuffer::copy(shared_point));
    return premaster_key;
}

}
