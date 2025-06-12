/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Optional.h>

namespace AK {

template<typename ValueT, typename ErrorT>
class [[nodiscard]] Result {
public:
    using ValueType = ValueT;
    using ErrorType = ErrorT;

    Result(ValueType const& res)
        : m_result(res)
    {
    }

    Result(ValueType&& res)
        : m_result(move(res))
    {
    }

    Result(ErrorType const& error)
        : m_error(error)
    {
    }

    Result(ErrorType&& error)
        : m_error(move(error))
    {
    }

    Result(Result&& other) = default;
    Result(Result const& other) = default;
    ~Result() = default;

    ValueType& value()
    {
        return m_result.value();
    }

    ErrorType& error()
    {
        return m_error.value();
    }

    bool is_error() const
    {
        return m_error.has_value();
    }

    ValueType release_value()
    {
        return m_result.release_value();
    }

    ErrorType release_error()
    {
        return m_error.release_value();
    }

private:
    Optional<ValueType> m_result;
    Optional<ErrorType> m_error;
};

// Partial specialization for void value type
template<typename ErrorT>
class [[nodiscard]] Result<void, ErrorT> {
public:
    using ValueType = void;
    using ErrorType = ErrorT;

    Result(ErrorType const& error)
        : m_error(error)
    {
    }

    Result(ErrorType&& error)
        : m_error(move(error))
    {
    }

    Result() = default;
    Result(Result&& other) = default;
    Result(Result const& other) = default;
    ~Result() = default;

    // For compatibility with TRY().
    void value() { }
    void release_value() { }

    ErrorType& error()
    {
        return m_error.value();
    }

    bool is_error() const
    {
        return m_error.has_value();
    }

    ErrorType release_error()
    {
        return m_error.release_value();
    }

private:
    Optional<ErrorType> m_error;
};

}

#if USING_AK_GLOBALLY
using AK::Result;
#endif
