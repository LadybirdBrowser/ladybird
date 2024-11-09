/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/Types.h>
#include <LibCrypto/Checksum/ChecksumFunction.h>

namespace Crypto::Checksum {

// A generic 16-bit Cyclic Redundancy Check.
// Just like CRC32, this class receives its polynomial little-endian.
// For example, the polynomial x¹⁶ + x¹² + x⁵ + 1 is represented as 0x8408.
template<u16 polynomial>
class CRC16 : public ChecksumFunction<u16> {
public:
    static constexpr u16 be_polynomial = bitswap(polynomial);

    // This is a big endian table, while CRC-32 uses a little endian table.
    static constexpr auto generate_table()
    {
        Array<u16, 256> data {};
        data[0] = 0;
        u16 value = 0x8000;
        auto i = 1u;
        do {
            if ((value & 0x8000) != 0) {
                value = be_polynomial ^ (value << 1);
            } else {
                value = value << 1;
            }

            for (auto j = 0u; j < i; ++j) {
                data[i + j] = value ^ data[j];
            }
            i <<= 1;
        } while (i < 256);

        return data;
    }

    static constexpr auto table = generate_table();

    virtual ~CRC16() = default;

    CRC16() = default;
    CRC16(ReadonlyBytes data)
    {
        update(data);
    }

    CRC16(u16 initial_state, ReadonlyBytes data)
        : m_state(initial_state)
    {
        update(data);
    }

    // FIXME: This implementation is naive and slow.
    //        Figure out how to adopt the slicing-by-8 algorithm (see CRC32) for 16-bit polynomials.
    virtual void update(ReadonlyBytes data) override
    {
        for (size_t i = 0; i < data.size(); i++) {
            size_t table_index = ((m_state >> 8) ^ data.at(i)) & 0xFF;
            m_state = (table[table_index] ^ (static_cast<u32>(m_state) << 8)) & 0xFFFF;
        }
    }

    virtual u16 digest() override
    {
        return m_state;
    }

private:
    u16 m_state { 0 };
};

}
