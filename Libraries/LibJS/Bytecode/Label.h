/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS::Bytecode {

class Label {
public:
    explicit Label(u32 address)
        : m_address(address)
    {
    }

    size_t address() const { return m_address; }

    void set_address(size_t address) { m_address = address; }

private:
    u32 m_address { 0 };
};

}
