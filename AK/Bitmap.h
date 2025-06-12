/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitmapView.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

namespace AK {

class Bitmap : public BitmapView {
    AK_MAKE_NONCOPYABLE(Bitmap);

public:
    static ErrorOr<Bitmap> create(size_t size, bool default_value)
    {
        VERIFY(size != 0);

        auto* data = kmalloc(ceil_div(size, static_cast<size_t>(8)));
        if (!data)
            return Error::from_errno(ENOMEM);

        auto bitmap = Bitmap { static_cast<u8*>(data), size, true };
        bitmap.fill(default_value);
        return bitmap;
    }

    Bitmap() = default;

    Bitmap(u8* data, size_t size, bool is_owning = false)
        : BitmapView(data, size)
        , m_is_owning(is_owning)
    {
    }

    Bitmap(Bitmap&& other)
        : BitmapView(exchange(other.m_data, nullptr), exchange(other.m_size, 0))
    {
        m_is_owning = exchange(other.m_is_owning, false);
    }

    Bitmap& operator=(Bitmap&& other)
    {
        if (this != &other) {
            kfree_sized(m_data, size_in_bytes());
            m_data = exchange(other.m_data, nullptr);
            m_size = exchange(other.m_size, 0);
        }
        return *this;
    }

    ~Bitmap()
    {
        if (m_is_owning) {
            kfree_sized(m_data, size_in_bytes());
        }
        m_data = nullptr;
    }

    [[nodiscard]] BitmapView view() const { return *this; }

    void set(size_t index, bool value)
    {
        VERIFY(index < m_size);
        if (value)
            m_data[index / 8] |= static_cast<u8>((1u << (index % 8)));
        else
            m_data[index / 8] &= static_cast<u8>(~(1u << (index % 8)));
    }

    // NOTE: There's a const method variant of this method at the parent class BitmapView.
    [[nodiscard]] u8* data() { return m_data; }

    void grow(size_t size, bool default_value)
    {
        VERIFY(size > m_size);

        auto previous_size_bytes = size_in_bytes();
        auto previous_size = m_size;
        auto* previous_data = m_data;

        m_size = size;
        m_data = reinterpret_cast<u8*>(kmalloc(size_in_bytes()));

        fill(default_value);

        if (previous_data != nullptr) {
            __builtin_memcpy(m_data, previous_data, previous_size_bytes);
            if ((previous_size % 8) != 0)
                set_range(previous_size, 8 - previous_size % 8, default_value);
            kfree_sized(previous_data, previous_size_bytes);
        }
    }

    template<bool VALUE, bool verify_that_all_bits_flip = false>
    void set_range(size_t start, size_t len)
    {
        VERIFY(start < m_size);
        VERIFY(start + len <= m_size);
        if (len == 0)
            return;

        u8* first = &m_data[start / 8];
        u8* last = &m_data[(start + len) / 8];
        u8 byte_mask = bitmask_first_byte[start % 8];
        if (first == last) {
            byte_mask &= bitmask_last_byte[(start + len) % 8];
            if constexpr (verify_that_all_bits_flip) {
                if constexpr (VALUE) {
                    VERIFY((*first & byte_mask) == 0);
                } else {
                    VERIFY((*first & byte_mask) == byte_mask);
                }
            }
            if constexpr (VALUE)
                *first |= byte_mask;
            else
                *first &= ~byte_mask;
        } else {
            if constexpr (verify_that_all_bits_flip) {
                if constexpr (VALUE) {
                    VERIFY((*first & byte_mask) == 0);
                } else {
                    VERIFY((*first & byte_mask) == byte_mask);
                }
            }
            if constexpr (VALUE)
                *first |= byte_mask;
            else
                *first &= ~byte_mask;
            byte_mask = bitmask_last_byte[(start + len) % 8];
            if constexpr (verify_that_all_bits_flip) {
                if constexpr (VALUE) {
                    VERIFY((*last & byte_mask) == 0);
                } else {
                    VERIFY((*last & byte_mask) == byte_mask);
                }
            }
            if constexpr (VALUE)
                *last |= byte_mask;
            else
                *last &= ~byte_mask;
            if (++first < last) {
                if constexpr (VALUE)
                    __builtin_memset(first, 0xFF, last - first);
                else
                    __builtin_memset(first, 0x0, last - first);
            }
        }
    }

    void set_range(size_t start, size_t len, bool value)
    {
        if (value)
            set_range<true, false>(start, len);
        else
            set_range<false, false>(start, len);
    }

    void set_range_and_verify_that_all_bits_flip(size_t start, size_t len, bool value)
    {
        if (value)
            set_range<true, true>(start, len);
        else
            set_range<false, true>(start, len);
    }

    void fill(bool value)
    {
        __builtin_memset(m_data, value ? 0xff : 0x00, size_in_bytes());
    }

private:
    bool m_is_owning { true };
};

}

#if USING_AK_GLOBALLY
using AK::Bitmap;
#endif
