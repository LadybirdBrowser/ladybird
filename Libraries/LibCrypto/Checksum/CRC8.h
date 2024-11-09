/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibCrypto/Checksum/ChecksumFunction.h>

namespace Crypto::Checksum {

// A generic 8-bit Cyclic Redundancy Check.
// Note that as opposed to CRC32, this class operates with MSB first, so the polynomial must not be reversed.
// For example, the polynomial x⁸ + x² + x + 1 is represented as 0x07 and not 0xE0.
template<u8 polynomial>
class CRC8 : public ChecksumFunction<u8> {
public:
    // This is a big endian table, while CRC-32 uses a little endian table.
    static constexpr auto generate_table()
    {
        Array<u8, 256> data {};
        u8 value = 0x80;
        auto i = 1u;
        do {
            if ((value & 0x80) != 0) {
                value = polynomial ^ (value << 1);
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

    virtual ~CRC8() = default;

    CRC8() = default;
    CRC8(ReadonlyBytes data)
    {
        update(data);
    }

    CRC8(u8 initial_state, ReadonlyBytes data)
        : m_state(initial_state)
    {
        update(data);
    }

    // FIXME: This implementation is naive and slow.
    //        Figure out how to adopt the slicing-by-8 algorithm (see CRC32) for 8 bit polynomials.
    virtual void update(ReadonlyBytes data) override
    {
        for (size_t i = 0; i < data.size(); i++) {
            size_t table_index = (m_state ^ data.at(i)) & 0xFF;
            m_state = (table[table_index] ^ (static_cast<u32>(m_state) << 8)) & 0xFF;
        }
    }

    virtual u8 digest() override
    {
        return m_state;
    }

private:
    u8 m_state { 0 };
};

}
