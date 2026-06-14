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
    [[nodiscard]] bool operator==(Operand const&) const = default;

    enum class ShouldMakeInvalid { Indeed };
    explicit Operand(ShouldMakeInvalid)
        : m_raw(0xffffffffu)
    {
    }

    [[nodiscard]] bool is_invalid() const { return m_raw == 0xffffffffu; }
    [[nodiscard]] u32 raw() const { return m_raw; }

private:
    Operand() = default;

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
