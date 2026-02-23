/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/Concepts.h>
#include <AK/Forward.h>
#include <AK/HashFunctions.h>
#include <AK/StringHash.h>

namespace AK {

template<typename T>
struct DefaultTraits {
    using PeekType = T&;
    using ConstPeekType = T const&;
    static constexpr bool is_trivial() { return false; }
    static constexpr bool is_trivially_serializable() { return false; }
    static constexpr bool equals(T const& a, T const& b) { return a == b; }
    template<Concepts::HashCompatible<T> U>
    static bool equals(T const& self, U const& other) { return self == other; }
    // NOTE: Override this to say false if your type has a fast equality check.
    //       If equality checks are fast, we won't store hashes in HashTable/HashMap,
    static constexpr bool may_have_slow_equality_check() { return true; }
};

template<typename T>
struct Traits : public DefaultTraits<T> {
};

template<typename T>
struct Traits<T const> : public Traits<T> {
    using PeekType = typename Traits<T>::ConstPeekType;
};

template<Integral T>
struct Traits<T> : public DefaultTraits<T> {
    static constexpr bool is_trivial() { return true; }
    static constexpr bool is_trivially_serializable() { return true; }
    // NOTE: Trivial types always have fast equality checks.
    static constexpr bool may_have_slow_equality_check() { return false; }
    static unsigned hash(T value)
    {
        if constexpr (sizeof(T) < 8)
            return u32_hash(value);
        else
            return u64_hash(value);
    }
};

template<FloatingPoint T>
struct Traits<T> : public DefaultTraits<T> {
    static constexpr bool is_trivial() { return true; }
    static constexpr bool is_trivially_serializable() { return true; }
    static constexpr bool may_have_slow_equality_check() { return false; }
    static unsigned hash(T value)
    {
        if constexpr (sizeof(T) < 8)
            return u32_hash(bit_cast<u32>(value));
        else
            return u64_hash(bit_cast<u64>(value));
    }
};

template<typename T>
requires(IsPointer<T> && !Detail::IsPointerOfType<char, T>) struct Traits<T> : public DefaultTraits<T> {
    static unsigned hash(T p) { return ptr_hash(bit_cast<FlatPtr>(p)); }
    static constexpr bool is_trivial() { return true; }
    // NOTE: Trivial types always have fast equality checks.
    static constexpr bool may_have_slow_equality_check() { return false; }
};

template<Enum T>
struct Traits<T> : public DefaultTraits<T> {
    static unsigned hash(T value) { return Traits<UnderlyingType<T>>::hash(to_underlying(value)); }
    static constexpr bool is_trivial() { return Traits<UnderlyingType<T>>::is_trivial(); }
    // NOTE: Trivial types always have fast equality checks.
    static constexpr bool may_have_slow_equality_check() { return !is_trivial(); }
    static constexpr bool is_trivially_serializable() { return Traits<UnderlyingType<T>>::is_trivially_serializable(); }
};

template<Integral T>
struct IdentityHashTraits : public Traits<T> {
    static constexpr unsigned hash(T value)
    {
        if constexpr (sizeof(T) <= 4)
            return static_cast<unsigned>(value);
        else
            return static_cast<unsigned>(value ^ (value >> 32));
    }
};

}

#if USING_AK_GLOBALLY
using AK::DefaultTraits;
using AK::IdentityHashTraits;
using AK::Traits;
#endif
