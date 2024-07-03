/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NumericLimits.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCrypto/Checksum/CRC32.h>

#ifdef __ARM_ACLE
#    include <arm_acle.h>
#endif

namespace Crypto::Checksum {

#if __ARM_ARCH >= 8 && defined(__ARM_FEATURE_CRC32) && defined(__ARM_ACLE)
void CRC32::update(ReadonlyBytes span)
{
    // FIXME: Does this require runtime checking on rpi?
    //        (Maybe the instruction is present on the rpi4 but not on the rpi3?)

    u8 const* data = span.data();
    size_t size = span.size();

    while (size > 0 && (reinterpret_cast<FlatPtr>(data) & 7) != 0) {
        m_state = __crc32b(m_state, *data);
        ++data;
        --size;
    }

    auto* data64 = reinterpret_cast<u64 const*>(data);
    while (size >= 8) {
        m_state = __crc32d(m_state, *data64);
        ++data64;
        size -= 8;
    }

    data = reinterpret_cast<u8 const*>(data64);
    while (size > 0) {
        m_state = __crc32b(m_state, *data);
        ++data;
        --size;
    }
}

// FIXME: On Intel, use _mm_crc32_u8 / _mm_crc32_u64 if available (SSE 4.2).

#else

static constexpr size_t ethernet_polynomial = 0xEDB88320;

#    if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

// This implements Intel's slicing-by-8 algorithm. Their original paper is no longer on their website,
// but their source code is still available for reference:
// https://sourceforge.net/projects/slicing-by-8/
static constexpr auto generate_table()
{
    Array<Array<u32, 256>, 8> data {};

    for (u32 i = 0; i < 256; ++i) {
        auto value = i;

        for (size_t j = 0; j < 8; ++j)
            value = (value >> 1) ^ ((value & 1) * ethernet_polynomial);

        data[0][i] = value;
    }

    for (u32 i = 0; i < 256; ++i) {
        for (size_t j = 1; j < 8; ++j)
            data[j][i] = (data[j - 1][i] >> 8) ^ data[0][data[j - 1][i] & 0xff];
    }

    return data;
}

static constexpr auto table = generate_table();

struct AlignmentData {
    ReadonlyBytes misaligned;
    ReadonlyBytes aligned;
};

static AlignmentData split_bytes_for_alignment(ReadonlyBytes data, size_t alignment)
{
    auto address = reinterpret_cast<uintptr_t>(data.data());
    auto offset = alignment - address % alignment;

    if (offset == alignment)
        return { {}, data };

    if (data.size() < alignment)
        return { data, {} };

    return { data.trim(offset), data.slice(offset) };
}

static constexpr u32 single_byte_crc(u32 crc, u8 byte)
{
    return (crc >> 8) ^ table[0][(crc & 0xff) ^ byte];
}

void CRC32::update(ReadonlyBytes data)
{
    // The provided data may not be aligned to a 4-byte boundary, required to reinterpret its address
    // into a u32 in the loop below. So we split the bytes into two segments: the misaligned bytes
    // (which undergo the standard 1-byte-at-a-time algorithm) and remaining aligned bytes.
    auto [misaligned_data, aligned_data] = split_bytes_for_alignment(data, alignof(u32));

    for (auto byte : misaligned_data)
        m_state = single_byte_crc(m_state, byte);

    while (aligned_data.size() >= 8) {
        auto const* segment = reinterpret_cast<u32 const*>(aligned_data.data());
        auto low = *segment ^ m_state;
        auto high = *(++segment);

        m_state = table[0][(high >> 24) & 0xff]
            ^ table[1][(high >> 16) & 0xff]
            ^ table[2][(high >> 8) & 0xff]
            ^ table[3][high & 0xff]
            ^ table[4][(low >> 24) & 0xff]
            ^ table[5][(low >> 16) & 0xff]
            ^ table[6][(low >> 8) & 0xff]
            ^ table[7][low & 0xff];

        aligned_data = aligned_data.slice(8);
    }

    for (auto byte : aligned_data)
        m_state = single_byte_crc(m_state, byte);
}

#    else

// FIXME: Implement the slicing-by-8 algorithm for big endian CPUs.
static constexpr auto generate_table()
{
    Array<u32, 256> data {};
    for (auto i = 0u; i < data.size(); i++) {
        u32 value = i;

        for (auto j = 0; j < 8; j++) {
            if (value & 1) {
                value = ethernet_polynomial ^ (value >> 1);
            } else {
                value = value >> 1;
            }
        }

        data[i] = value;
    }
    return data;
}

static constexpr auto table = generate_table();

void CRC32::update(ReadonlyBytes data)
{
    for (size_t i = 0; i < data.size(); i++) {
        m_state = table[(m_state ^ data.at(i)) & 0xFF] ^ (m_state >> 8);
    }
}

#    endif
#endif

u32 CRC32::digest()
{
    return ~m_state;
}

}
