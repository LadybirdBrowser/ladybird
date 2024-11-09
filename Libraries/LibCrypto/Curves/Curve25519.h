/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Random.h>

namespace Crypto::Curves {

class Curve25519 {
public:
    static constexpr u8 BASE_POINT_L_ORDER[33] {
        0xED, 0xD3, 0xF5, 0x5C, 0x1A, 0x63, 0x12, 0x58,
        0xD6, 0x9C, 0xF7, 0xA2, 0xDE, 0xF9, 0xDE, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
        0x00
    };

    static constexpr u32 CURVE_D[8] {
        0x135978A3, 0x75EB4DCA, 0x4141D8AB, 0x00700A4D,
        0x7779E898, 0x8CC74079, 0x2B6FFE73, 0x52036CEE
    };

    static constexpr u32 CURVE_D_2[8] {
        0x26B2F159, 0xEBD69B94, 0x8283B156, 0x00E0149A,
        0xEEF3D130, 0x198E80F2, 0x56DFFCE7, 0x2406D9DC
    };

    static constexpr u32 ZERO[8] {
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    };

    static constexpr u32 SQRT_MINUS_1[8] {
        0x4A0EA0B0, 0xC4EE1B27, 0xAD2FE478, 0x2F431806,
        0x3DFBD7A7, 0x2B4D0099, 0x4FC1DF0B, 0x2B832480
    };

    static constexpr u8 BARRETT_REDUCTION_QUOTIENT[33] {
        0x1B, 0x13, 0x2C, 0x0A, 0xA3, 0xE5, 0x9C, 0xED,
        0xA7, 0x29, 0x63, 0x08, 0x5D, 0x21, 0x06, 0x21,
        0xEB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x0F
    };

    static constexpr u8 BITS = 255;
    static constexpr u8 BYTES = 32;
    static constexpr u8 WORDS = 8;
    static constexpr u32 A24 = 121666;

    static void set(u32* a, u32 b);
    static void select(u32* r, u32 const* a, u32 const* b, u32 c);
    static void copy(u32* a, u32 const* b);
    static void modular_square(u32* r, u32 const* a);
    static void modular_subtract(u32* r, u32 const* a, u32 const* b);
    static void modular_reduce(u32* r, u32 const* a);
    static void modular_add(u32* r, u32 const* a, u32 const* b);
    static void modular_multiply(u32* r, u32 const* a, u32 const* b);
    static void modular_multiply_inverse(u32* r, u32 const* a);
    static void to_power_of_2n(u32* r, u32 const* a, u8 n);
    static void export_state(u32* a, u8* data);
    static void import_state(u32* a, u8 const* data);
    static void modular_subtract_single(u32* r, u32 const* a, u32 b);
    static void modular_multiply_single(u32* r, u32 const* a, u32 b);
    static void modular_add_single(u32* r, u32 const* a, u32 b);
    static u32 modular_square_root(u32* r, u32 const* a, u32 const* b);
    static u32 compare(u32 const* a, u32 const* b);
};
}
