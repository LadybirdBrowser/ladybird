/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>

namespace JS::Bytecode {

class BasicBlock;

class Label {
public:
    explicit Label(BasicBlock const&);

    explicit Label(u32 basic_block_index)
        : m_address_or_basic_block_index(basic_block_index)
    {
    }

    // Used while compiling.
    size_t basic_block_index() const { return m_address_or_basic_block_index; }

    // Used after compiling.
    size_t address() const { return m_address_or_basic_block_index; }

    void set_address(size_t address) { m_address_or_basic_block_index = address; }

private:
    u32 m_address_or_basic_block_index { 0 };
};

}

template<>
struct AK::Formatter<JS::Bytecode::Label> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, JS::Bytecode::Label const& label)
    {
        return AK::Formatter<FormatString>::format(builder, "@{:x}"sv, label.address());
    }
};
