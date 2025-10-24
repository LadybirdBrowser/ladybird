/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Checked.h>
#include <AK/Error.h>
#include <AK/NumericLimits.h>
#include <AK/Types.h>

namespace IPC::SafeMath {

// Safe arithmetic operations with overflow detection
// These functions prevent integer overflow vulnerabilities in IPC message handling

// Safe multiplication with overflow detection
template<typename T>
requires(IsIntegral<T>)
ErrorOr<T> checked_mul(T a, T b)
{
    Checked<T> result = a;
    result *= b;

    if (result.has_overflow())
        return Error::from_string_literal("Integer multiplication overflow");

    return result.value();
}

// Safe addition with overflow detection
template<typename T>
requires(IsIntegral<T>)
ErrorOr<T> checked_add(T a, T b)
{
    Checked<T> result = a;
    result += b;

    if (result.has_overflow())
        return Error::from_string_literal("Integer addition overflow");

    return result.value();
}

// Safe subtraction with underflow detection
template<typename T>
requires(IsIntegral<T>)
ErrorOr<T> checked_sub(T a, T b)
{
    Checked<T> result = a;
    result -= b;

    if (result.has_overflow())
        return Error::from_string_literal("Integer subtraction underflow");

    return result.value();
}

// Safe size calculation for image buffers (width * height * bytes_per_pixel)
// This is a common pattern in image decoding that must be overflow-safe
ErrorOr<size_t> calculate_buffer_size(u32 width, u32 height, u32 bytes_per_pixel)
{
    // Check for zero dimensions (should not allocate)
    if (width == 0 || height == 0 || bytes_per_pixel == 0)
        return Error::from_string_literal("Invalid dimensions: zero size");

    // Calculate row size: width * bytes_per_pixel
    auto row_size = TRY(checked_mul<size_t>(width, bytes_per_pixel));

    // Calculate total size: row_size * height
    auto total_size = TRY(checked_mul<size_t>(row_size, height));

    return total_size;
}

// Validate size fits in target type (safe narrowing cast)
template<typename Target, typename Source>
requires(IsIntegral<Target> && IsIntegral<Source>)
ErrorOr<Target> safe_cast(Source value)
{
    // Check if value is within range of target type
    if constexpr (IsSigned<Source> && IsUnsigned<Target>) {
        // Signed to unsigned: check for negative
        if (value < 0)
            return Error::from_string_literal("Cannot cast negative value to unsigned type");
    }

    // Check upper bound
    if (value > static_cast<Source>(NumericLimits<Target>::max()))
        return Error::from_string_literal("Value exceeds maximum for target type");

    // Check lower bound (for signed types)
    if constexpr (IsSigned<Target>) {
        if (value < static_cast<Source>(NumericLimits<Target>::min()))
            return Error::from_string_literal("Value below minimum for target type");
    }

    return static_cast<Target>(value);
}

// Align size to alignment boundary (commonly used for buffer allocation)
// Returns aligned size or error if overflow occurs
ErrorOr<size_t> align_size(size_t size, size_t alignment)
{
    if (alignment == 0)
        return Error::from_string_literal("Alignment cannot be zero");

    // Check if alignment is power of 2
    if ((alignment & (alignment - 1)) != 0)
        return Error::from_string_literal("Alignment must be power of 2");

    // Calculate aligned size: (size + alignment - 1) & ~(alignment - 1)
    auto mask = alignment - 1;
    auto size_plus_mask = TRY(checked_add(size, mask));
    auto aligned = size_plus_mask & ~mask;

    return aligned;
}

// Calculate array size with element count and element size
// Returns total bytes needed or error if overflow
template<typename T>
ErrorOr<size_t> calculate_array_size(size_t count)
{
    return checked_mul(count, sizeof(T));
}

// Validate index is within bounds of array/vector
ErrorOr<void> validate_index(size_t index, size_t size)
{
    if (index >= size)
        return Error::from_string_literal("Index out of bounds");

    return {};
}

// Validate range [start, end) is within bounds
ErrorOr<void> validate_range(size_t start, size_t end, size_t size)
{
    if (start > end)
        return Error::from_string_literal("Invalid range: start > end");

    if (end > size)
        return Error::from_string_literal("Range exceeds size");

    return {};
}

}
