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
    enum class Type
#if ARCH(AARCH64)
        : u8
#endif
    {
        Invalid,
        Register,
        Local,
        Constant,
        Argument,
    };

    [[nodiscard]] bool operator==(Operand const&) const = default;

    explicit Operand(Type type, u32 index)
        : m_type(type)
        , m_index(index)
    {
#if ARCH(AARCH64)
        VERIFY((index & 0x3fffffff) == index);
#endif
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
    // NOTE: aarch64 gets much faster with bitfields here, while x86_64 gets slower.
    //       Because this type is absolutely essential to the interpreter, we allow
    //       ourselves this little ifdef.
#if ARCH(AARCH64)
    Type m_type : 3 {};
    u32 m_index : 29 { 0 };
#else
    Type m_type { Type::Invalid };
    u32 m_index { 0 };
#endif
};

#if ARCH(AARCH64)
static_assert(sizeof(Operand) == 4);
#else
static_assert(sizeof(Operand) == 8);
#endif

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
        m_value = JS::Bytecode::Operand { JS::Bytecode::Operand::Type::Invalid, 0 };
    }

    [[nodiscard]] bool has_value() const
    {
        return m_value.type() != JS::Bytecode::Operand::Type::Invalid;
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
    JS::Bytecode::Operand m_value { JS::Bytecode::Operand::Type::Invalid, 0 };
};

}
