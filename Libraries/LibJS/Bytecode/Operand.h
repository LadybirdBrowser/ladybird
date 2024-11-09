/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibJS/Forward.h>

namespace JS::Bytecode {

class Operand {
public:
    enum class Type {
        Register,
        Local,
        Constant,
    };

    [[nodiscard]] bool operator==(Operand const&) const = default;

    explicit Operand(Type type, u32 index)
        : m_type(type)
        , m_index(index)
    {
    }

    explicit Operand(Register);

    [[nodiscard]] bool is_register() const { return m_type == Type::Register; }
    [[nodiscard]] bool is_local() const { return m_type == Type::Local; }
    [[nodiscard]] bool is_constant() const { return m_type == Type::Constant; }

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] u32 index() const { return m_index; }

    [[nodiscard]] Register as_register() const;

    void offset_index_by(u32 offset) { m_index += offset; }

private:
    Type m_type {};
    u32 m_index { 0 };
};

}
