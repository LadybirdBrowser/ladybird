/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Forward.h>
#include <AK/Math.h>
#include <AK/Types.h>
#include <math.h>

namespace Audio::QOA {

// 'qoaf'
static constexpr u32 const magic = 0x716f6166;

static constexpr size_t const header_size = sizeof(u64);

struct FrameHeader {
    u8 num_channels;
    u32 sample_rate; // 24 bits
    u16 sample_count;
    // TODO: might be removed and/or replaced
    u16 frame_size;

    static ErrorOr<FrameHeader> read_from_stream(Stream& stream);
};

static constexpr size_t const frame_header_size = sizeof(u64);

// Least mean squares (LMS) predictor FIR filter size.
static constexpr size_t const lms_history = 4;

static constexpr size_t const lms_state_size = 2 * lms_history * sizeof(u16);

// Only used for internal purposes; intermediate LMS states can be beyond 16 bits.
struct LMSState {
    i32 history[lms_history] { 0, 0, 0, 0 };
    i32 weights[lms_history] { 0, 0, 0, 0 };

    LMSState() = default;
    LMSState(u64 history_packed, u64 weights_packed);

    i32 predict() const;
    void update(i32 sample, i32 residual);
};

using PackedSlice = u64;

// A QOA slice in a more directly readable format, unpacked from the stored 64-bit format.
struct UnpackedSlice {
    size_t scale_factor_index; // 4 bits packed
    Array<u8, 20> residuals;   // 3 bits packed
};

// Samples within a 64-bit slice.
static constexpr size_t const slice_samples = 20;
static constexpr size_t const max_slices_per_frame = 256;
static constexpr size_t const max_frame_samples = slice_samples * max_slices_per_frame;

// Defined as clamping limits by the spec.
static constexpr i32 const sample_minimum = -32768;
static constexpr i32 const sample_maximum = 32767;

// Quantization and scale factor tables computed from formulas given in qoa.h

constexpr Array<int, 17> generate_scale_factor_table()
{
    Array<int, 17> scalefactor_table;
    for (size_t s = 0; s < 17; ++s)
        scalefactor_table[s] = static_cast<int>(AK::round<double>(AK::pow<double>(static_cast<double>(s + 1), 2.75)));

    return scalefactor_table;
}

// FIXME: Get rid of the literal table once Clang understands our constexpr pow() and round() implementations.
#if defined(AK_COMPILER_CLANG)
static constexpr Array<int, 17> scale_factor_table = {
    1, 7, 21, 45, 84, 138, 211, 304, 421, 562, 731, 928, 1157, 1419, 1715, 2048
};
#else
static constexpr Array<int, 17> scale_factor_table = generate_scale_factor_table();
#endif

constexpr Array<int, 17> generate_reciprocal_table()
{
    Array<int, 17> reciprocal_table;
    for (size_t s = 0; s < 17; ++s) {
        reciprocal_table[s] = ((1 << 16) + scale_factor_table[s] - 1) / scale_factor_table[s];
    }
    return reciprocal_table;
}

constexpr Array<Array<int, 8>, 16> generate_dequantization_table()
{
    Array<Array<int, 8>, 16> dequantization_table;
    constexpr Array<double, 8> float_dequantization_table = { 0.75, -0.75, 2.5, -2.5, 4.5, -4.5, 7, -7 };
    for (size_t scale = 0; scale < 16; ++scale) {
        for (size_t quantization = 0; quantization < 8; ++quantization)
            dequantization_table[scale][quantization] = static_cast<int>(AK::round<double>(
                static_cast<double>(scale_factor_table[scale]) * float_dequantization_table[quantization]));
    }
    return dequantization_table;
}

#if defined(AK_COMPILER_CLANG)
static constexpr Array<int, 17> reciprocal_table = {
    65536, 9363, 3121, 1457, 781, 475, 311, 216, 156, 117, 90, 71, 57, 47, 39, 32
};
static constexpr Array<Array<int, 8>, 16> dequantization_table = {
    Array<int, 8> { 1, -1, 3, -3, 5, -5, 7, -7 },
    { 5, -5, 18, -18, 32, -32, 49, -49 },
    { 16, -16, 53, -53, 95, -95, 147, -147 },
    { 34, -34, 113, -113, 203, -203, 315, -315 },
    { 63, -63, 210, -210, 378, -378, 588, -588 },
    { 104, -104, 345, -345, 621, -621, 966, -966 },
    { 158, -158, 528, -528, 950, -950, 1477, -1477 },
    { 228, -228, 760, -760, 1368, -1368, 2128, -2128 },
    { 316, -316, 1053, -1053, 1895, -1895, 2947, -2947 },
    { 422, -422, 1405, -1405, 2529, -2529, 3934, -3934 },
    { 548, -548, 1828, -1828, 3290, -3290, 5117, -5117 },
    { 696, -696, 2320, -2320, 4176, -4176, 6496, -6496 },
    { 868, -868, 2893, -2893, 5207, -5207, 8099, -8099 },
    { 1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933 },
    { 1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005 },
    { 1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336 },
};
#else
static constexpr Array<int, 17> reciprocal_table = generate_reciprocal_table();
static constexpr Array<Array<int, 8>, 16> dequantization_table = generate_dequantization_table();
#endif

static constexpr Array<int, 17> quantization_table = {
    7, 7, 7, 5, 5, 3, 3, 1, // -8 ..-1
    0,                      //  0
    0, 2, 2, 4, 4, 6, 6, 6  //  1 .. 8
};

}
