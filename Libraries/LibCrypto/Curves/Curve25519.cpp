/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/Types.h>
#include <LibCrypto/Curves/Curve25519.h>

namespace Crypto::Curves {

void Curve25519::set(u32* state, u32 value)
{
    state[0] = value;

    for (auto i = 1; i < WORDS; i++) {
        state[i] = 0;
    }
}

void Curve25519::modular_square(u32* state, u32 const* value)
{
    // Compute R = (A ^ 2) mod p
    modular_multiply(state, value, value);
}

void Curve25519::modular_subtract(u32* state, u32 const* first, u32 const* second)
{
    // R = (A - B) mod p
    i64 temp = -19;
    for (auto i = 0; i < WORDS; i++) {
        temp += first[i];
        temp -= second[i];
        state[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    // Compute R = A + (2^255 - 19) - B
    state[7] += 0x80000000;

    modular_reduce(state, state);
}

void Curve25519::modular_add(u32* state, u32 const* first, u32 const* second)
{
    // R = (A + B) mod p
    u64 temp = 0;
    for (auto i = 0; i < WORDS; i++) {
        temp += first[i];
        temp += second[i];
        state[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    modular_reduce(state, state);
}

void Curve25519::modular_multiply(u32* state, u32 const* first, u32 const* second)
{
    // Compute R = (A * B) mod p
    u64 temp = 0;
    u64 carry = 0;
    u32 output[WORDS * 2];

    // Comba's method
    for (auto i = 0; i < 16; i++) {
        if (i < WORDS) {
            for (auto j = 0; j <= i; j++) {
                temp += (u64)first[j] * second[i - j];
                carry += temp >> 32;
                temp &= 0xFFFFFFFF;
            }
        } else {
            for (auto j = i - 7; j < WORDS; j++) {
                temp += (u64)first[j] * second[i - j];
                carry += temp >> 32;
                temp &= 0xFFFFFFFF;
            }
        }

        output[i] = temp & 0xFFFFFFFF;
        temp = carry & 0xFFFFFFFF;
        carry >>= 32;
    }

    // Reduce bit 255 (2^255 = 19 mod p)
    temp = (output[7] >> 31) * 19;
    // Mask the most significant bit
    output[7] &= 0x7FFFFFFF;

    // Fast modular reduction 1st pass
    for (auto i = 0; i < WORDS; i++) {
        temp += output[i];
        temp += (u64)output[i + 8] * 38;
        output[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    // Reduce bit 256 (2^256 = 38 mod p)
    temp *= 38;
    // Reduce bit 255 (2^255 = 19 mod p)
    temp += (output[7] >> 31) * 19;
    // Mask the most significant bit
    output[7] &= 0x7FFFFFFF;

    // Fast modular reduction 2nd pass
    for (auto i = 0; i < WORDS; i++) {
        temp += output[i];
        output[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    modular_reduce(state, output);
}

void Curve25519::export_state(u32* state, u8* output)
{
    for (u32 i = 0; i < WORDS; i++) {
        state[i] = AK::convert_between_host_and_little_endian(state[i]);
    }

    memcpy(output, state, BYTES);
}

void Curve25519::import_state(u32* state, u8 const* data)
{
    memcpy(state, data, BYTES);
    for (u32 i = 0; i < WORDS; i++) {
        state[i] = AK::convert_between_host_and_little_endian(state[i]);
    }
}

void Curve25519::modular_subtract_single(u32* r, u32 const* a, u32 b)
{
    i64 temp = -19;
    temp -= b;

    // Compute R = A - 19 - B
    for (u32 i = 0; i < 8; i++) {
        temp += a[i];
        r[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    // Compute R = A + (2^255 - 19) - B
    r[7] += 0x80000000;
    modular_reduce(r, r);
}

void Curve25519::modular_add_single(u32* state, u32 const* first, u32 second)
{
    u64 temp = second;

    // Compute R = A + B
    for (u32 i = 0; i < 8; i++) {
        temp += first[i];
        state[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    modular_reduce(state, state);
}

u32 Curve25519::modular_square_root(u32* r, u32 const* a, u32 const* b)
{
    u32 c[8];
    u32 u[8];
    u32 v[8];

    // To compute the square root of (A / B), the first step is to compute the candidate root x = (A / B)^((p+3)/8)
    modular_square(v, b);
    modular_multiply(v, v, b);
    modular_square(v, v);
    modular_multiply(v, v, b);
    modular_multiply(c, a, v);
    modular_square(u, c);
    modular_multiply(u, u, c);
    modular_square(u, u);
    modular_multiply(v, u, c);
    to_power_of_2n(u, v, 3);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, c);
    to_power_of_2n(u, v, 7);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, c);
    to_power_of_2n(u, v, 15);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, c);
    to_power_of_2n(u, v, 31);
    modular_multiply(v, u, v);
    to_power_of_2n(u, v, 62);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, c);
    to_power_of_2n(u, v, 125);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_square(u, u);
    modular_multiply(u, u, c);

    // The first candidate root is U = A * B^3 * (A * B^7)^((p - 5) / 8)
    modular_multiply(u, u, a);
    modular_square(v, b);
    modular_multiply(v, v, b);
    modular_multiply(u, u, v);

    // The second candidate root is V = U * sqrt(-1)
    modular_multiply(v, u, SQRT_MINUS_1);

    modular_square(c, u);
    modular_multiply(c, c, b);

    // Check whether B * U^2 = A
    u32 first_comparison = compare(c, a);

    modular_square(c, v);
    modular_multiply(c, c, b);

    // Check whether B * V^2 = A
    u32 second_comparison = compare(c, a);

    // Select the first or the second candidate root
    select(r, u, v, first_comparison);

    // Return 0 if the square root exists
    return first_comparison & second_comparison;
}

u32 Curve25519::compare(u32 const* a, u32 const* b)
{
    u32 mask = 0;
    for (u32 i = 0; i < 8; i++) {
        mask |= a[i] ^ b[i];
    }

    // Return 0 if A = B, else 1
    return ((u32)(mask | (~mask + 1))) >> 31;
}

void Curve25519::modular_reduce(u32* state, u32 const* data)
{
    // R = A mod p
    u64 temp = 19;
    u32 other[WORDS];

    for (auto i = 0; i < WORDS; i++) {
        temp += data[i];
        other[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    // Compute B = A - (2^255 - 19)
    other[7] -= 0x80000000;

    u32 mask = (other[7] & 0x80000000) >> 31;
    select(state, other, data, mask);
}

void Curve25519::to_power_of_2n(u32* state, u32 const* value, u8 n)
{
    // Pre-compute (A ^ 2) mod p
    modular_square(state, value);

    // Compute R = (A ^ (2^n)) mod p
    for (u32 i = 1; i < n; i++) {
        modular_square(state, state);
    }
}

void Curve25519::select(u32* state, u32 const* a, u32 const* b, u32 condition)
{
    // If B < (2^255 - 19) then R = B, else R = A
    u32 mask = condition - 1;

    for (auto i = 0; i < WORDS; i++) {
        state[i] = (a[i] & mask) | (b[i] & ~mask);
    }
}

void Curve25519::copy(u32* state, u32 const* value)
{
    for (auto i = 0; i < WORDS; i++) {
        state[i] = value[i];
    }
}

void Curve25519::modular_multiply_inverse(u32* state, u32 const* value)
{
    // Compute R = A^-1 mod p
    u32 u[WORDS];
    u32 v[WORDS];

    // Fermat's little theorem
    modular_square(u, value);
    modular_multiply(u, u, value);
    modular_square(u, u);
    modular_multiply(v, u, value);
    to_power_of_2n(u, v, 3);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, value);
    to_power_of_2n(u, v, 7);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, value);
    to_power_of_2n(u, v, 15);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, value);
    to_power_of_2n(u, v, 31);
    modular_multiply(v, u, v);
    to_power_of_2n(u, v, 62);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_multiply(v, u, value);
    to_power_of_2n(u, v, 125);
    modular_multiply(u, u, v);
    modular_square(u, u);
    modular_square(u, u);
    modular_multiply(u, u, value);
    modular_square(u, u);
    modular_square(u, u);
    modular_multiply(u, u, value);
    modular_square(u, u);
    modular_multiply(state, u, value);
}

void Curve25519::modular_multiply_single(u32* state, u32 const* first, u32 second)
{
    // Compute R = (A * B) mod p
    u64 temp = 0;
    u32 output[WORDS];

    for (auto i = 0; i < WORDS; i++) {
        temp += (u64)first[i] * second;
        output[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    // Reduce bit 256 (2^256 = 38 mod p)
    temp *= 38;
    // Reduce bit 255 (2^255 = 19 mod p)
    temp += (output[7] >> 31) * 19;
    // Mask the most significant bit
    output[7] &= 0x7FFFFFFF;

    // Fast modular reduction
    for (auto i = 0; i < WORDS; i++) {
        temp += output[i];
        output[i] = temp & 0xFFFFFFFF;
        temp >>= 32;
    }

    modular_reduce(state, output);
}
}
