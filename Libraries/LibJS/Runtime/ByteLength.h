/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Variant.h>

namespace JS {

class ByteLength {
public:
    static ByteLength auto_() { return { Auto {} }; }
    static ByteLength detached() { return { Detached {} }; }

    ByteLength(u32 length)
        : m_length(length)
    {
    }

    bool is_auto() const { return m_length.has<Auto>(); }
    bool is_detached() const { return m_length.has<Detached>(); }

    u32 length() const
    {
        VERIFY(m_length.has<u32>());
        return m_length.get<u32>();
    }

private:
    struct Auto { };
    struct Detached { };

    using Length = Variant<Auto, Detached, u32>;

    ByteLength(Length length)
        : m_length(move(length))
    {
    }

    Length m_length;
};

}
