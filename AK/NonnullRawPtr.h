/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Traits.h>

namespace AK {

template<typename T>
requires(!IsLvalueReference<T> && !IsRvalueReference<T>) class [[nodiscard]] NonnullRawPtr {
public:
    using ValueType = T;

    NonnullRawPtr() = delete;

    NonnullRawPtr(T& other)
        : m_ptr(&other)
    {
    }

    NonnullRawPtr(NonnullRawPtr const& other)
        : m_ptr(other.m_ptr)
    {
    }

    NonnullRawPtr(NonnullRawPtr&& other)
        : m_ptr(other.m_ptr)
    {
    }

    NonnullRawPtr& operator=(NonnullRawPtr const& other)
    {
        m_ptr = other.m_ptr;

        return *this;
    }

    NonnullRawPtr& operator=(NonnullRawPtr&& other)
    {
        m_ptr = other.m_ptr;

        return *this;
    }

    operator bool() const = delete;
    bool operator!() const = delete;

    operator T&() { return *m_ptr; }
    operator T const&() const { return *m_ptr; }

    [[nodiscard]] ALWAYS_INLINE T& value() { return *m_ptr; }
    [[nodiscard]] ALWAYS_INLINE T const& value() const { return *m_ptr; }

    [[nodiscard]] ALWAYS_INLINE T& operator*() { return value(); }
    [[nodiscard]] ALWAYS_INLINE T const& operator*() const { return value(); }

    ALWAYS_INLINE RETURNS_NONNULL T* operator->() { return &value(); }
    ALWAYS_INLINE RETURNS_NONNULL T const* operator->() const { return &value(); }

private:
    T* m_ptr;
};

template<typename T>
struct Traits<NonnullRawPtr<T>> : public DefaultTraits<NonnullRawPtr<T>> {
    static unsigned hash(NonnullRawPtr<T> const& handle) { return Traits<T>::hash(handle); }
};

namespace Detail {
template<typename T>
inline constexpr bool IsHashCompatible<NonnullRawPtr<T>, T> = true;

template<typename T>
inline constexpr bool IsHashCompatible<T, NonnullRawPtr<T>> = true;

}

}

#if USING_AK_GLOBALLY
using AK::NonnullRawPtr;
#endif
