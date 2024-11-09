/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibJS/Bytecode/Operand.h>

namespace JS::Bytecode {

class ScopedOperandImpl : public RefCounted<ScopedOperandImpl> {
public:
    ScopedOperandImpl(Generator& generator, Operand operand)
        : m_generator(generator)
        , m_operand(operand)
    {
    }

    ~ScopedOperandImpl();

    [[nodiscard]] Operand const& operand() const { return m_operand; }
    [[nodiscard]] Operand& operand() { return m_operand; }

private:
    Generator& m_generator;
    Operand m_operand;
};

class ScopedOperand {
public:
    explicit ScopedOperand(Generator& generator, Operand operand)
        : m_impl(adopt_ref(*new ScopedOperandImpl(generator, operand)))
    {
    }

    [[nodiscard]] Operand const& operand() const { return m_impl->operand(); }
    [[nodiscard]] Operand& operand() { return m_impl->operand(); }
    operator Operand() const { return operand(); }

    [[nodiscard]] bool operator==(ScopedOperand const& other) const { return operand() == other.operand(); }

    [[nodiscard]] size_t ref_count() const { return m_impl->ref_count(); }

private:
    NonnullRefPtr<ScopedOperandImpl> m_impl;
};

}
