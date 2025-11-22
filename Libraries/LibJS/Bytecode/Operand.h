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
class Optional<JS::Bytecode::Operand> : public OptionalBase<JS::Bytecode::Operand> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = JS::Bytecode::Operand;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<JS::Bytecode::Operand> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(other.m_value)
    {
    }

    template<typename U = JS::Bytecode::Operand>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, JS::Bytecode::Operand>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<JS::Bytecode::Operand>> && IsConstructible<JS::Bytecode::Operand, U &&>)
        : m_value(forward<U>(value))
    {
    }

    template<SameAs<OptionalNone> V>
    Optional& operator=(V)
    {
        clear();
        return *this;
    }

    Optional& operator=(Optional const& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    void clear()
    {
        m_value = JS::Bytecode::Operand { JS::Bytecode::Operand::ShouldMakeInvalid::Indeed };
    }

    [[nodiscard]] bool has_value() const
    {
        return !m_value.is_invalid();
    }

    [[nodiscard]] JS::Bytecode::Operand& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::Operand const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::Operand value() &&
    {
        return release_value();
    }

    [[nodiscard]] JS::Bytecode::Operand release_value()
    {
        VERIFY(has_value());
        JS::Bytecode::Operand released_value = m_value;
        clear();
        return released_value;
    }

private:
    JS::Bytecode::Operand m_value { JS::Bytecode::Operand::ShouldMakeInvalid::Indeed };
};

}
