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
            kfree(m_data);
            m_data = exchange(other.m_data, nullptr);
            m_size = exchange(other.m_size, 0);
        }
        return *this;
    }

    ~Bitmap()
    {
        if (m_is_owning) {
            kfree(m_data);
        }
        m_data = nullptr;
    }

    void set(size_t index, bool value)
    {
        VERIFY(index < m_size);
        if (value)
            m_data[index / 8] |= static_cast<u8>((1u << (index % 8)));
        else
            m_data[index / 8] &= static_cast<u8>(~(1u << (index % 8)));
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
