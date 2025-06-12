/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Noncopyable.h>
#include <AK/Traits.h>

namespace AK {

template<typename T>
requires(!IsLvalueReference<T> && !IsRvalueReference<T>) class [[nodiscard]] NonnullRawPtr {
    AK_MAKE_DEFAULT_COPYABLE(NonnullRawPtr);
    AK_MAKE_DEFAULT_MOVABLE(NonnullRawPtr);

public:
    using ValueType = T;

    constexpr NonnullRawPtr() = delete;
    constexpr NonnullRawPtr(T const&&) = delete;

    constexpr NonnullRawPtr(T& other)
        : m_ptr(&other)
    {
    }

    constexpr operator bool() const = delete;
    constexpr bool operator!() const = delete;

    constexpr operator T&() { return *m_ptr; }
    constexpr operator T const&() const { return *m_ptr; }

    [[nodiscard]] ALWAYS_INLINE constexpr T& value() { return *m_ptr; }
    [[nodiscard]] ALWAYS_INLINE constexpr T const& value() const { return *m_ptr; }

    [[nodiscard]] ALWAYS_INLINE constexpr T& operator*() { return value(); }
    [[nodiscard]] ALWAYS_INLINE constexpr T const& operator*() const { return value(); }

    ALWAYS_INLINE RETURNS_NONNULL constexpr T* operator->() { return &value(); }
    ALWAYS_INLINE RETURNS_NONNULL constexpr T const* operator->() const { return &value(); }

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
