/*
 * Copyright (c) 2024-2025, Andreas Kling <andreas@ladybird.org>
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
        Argument,
    };

    [[nodiscard]] bool operator==(Operand const&) const = default;

    explicit Operand(Type type, u32 index)
        : m_raw(to_underlying(type) << 29 | index)
    {
    }

    enum class ShouldMakeInvalid { Indeed };
    explicit Operand(ShouldMakeInvalid)
        : m_raw(0xffffffffu)
    {
    }

    explicit Operand(Register);

    [[nodiscard]] bool is_invalid() const { return m_raw == 0xffffffffu; }
    [[nodiscard]] bool is_register() const { return type() == Type::Register; }
    [[nodiscard]] bool is_local() const { return type() == Type::Local; }
    [[nodiscard]] bool is_constant() const { return type() == Type::Constant; }

    [[nodiscard]] Type type() const { return static_cast<Type>((m_raw & 0xe0000000u) >> 29); }
    [[nodiscard]] u32 index() const { return m_raw & 0x1fffffff; }

    [[nodiscard]] u32 raw() const { return m_raw; }

    [[nodiscard]] Register as_register() const;

    void offset_index_by(u32 offset)
    {
        m_raw &= 0x1fffffff;
        m_raw += offset;
    }

private:
    u32 m_raw { 0 };
};

}

namespace AK {

template<>
struct SentinelOptionalTraits<JS::Bytecode::Operand> {
    static JS::Bytecode::Operand sentinel_value() { return JS::Bytecode::Operand { JS::Bytecode::Operand::ShouldMakeInvalid::Indeed }; }
    static bool is_sentinel(JS::Bytecode::Operand const& value) { return value.is_invalid(); }
};

template<>
class Optional<JS::Bytecode::Operand> : public SentinelOptional<JS::Bytecode::Operand> {
public:
    using SentinelOptional::SentinelOptional;
};

}
