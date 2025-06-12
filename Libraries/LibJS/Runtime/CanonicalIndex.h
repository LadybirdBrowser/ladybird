/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/NumericLimits.h>
#include <AK/Types.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

class CanonicalIndex {
public:
    enum class Type {
        Index,
        Numeric,
        Undefined,
    };

    CanonicalIndex(Type type, u32 index)
        : m_type(type)
        , m_index(index)
    {
    }

    template<FloatingPoint T>
    CanonicalIndex(Type type, T index) = delete;

    template<FloatingPoint T>
    static ThrowCompletionOr<CanonicalIndex> from_double(VM& vm, Type type, T index)
    {
        if (index < static_cast<double>(NumericLimits<u32>::min()))
            return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidIntegerIndex, index);
        if (index > static_cast<double>(NumericLimits<u32>::max()))
            return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidIntegerIndex, index);

        return CanonicalIndex { type, static_cast<u32>(index) };
    }

    u32 as_index() const
    {
        VERIFY(is_index());
        return m_index;
    }

    bool is_index() const { return m_type == Type::Index; }
    bool is_undefined() const { return m_type == Type::Undefined; }

private:
    Type m_type;
    u32 m_index;
};

}
