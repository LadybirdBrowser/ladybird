/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Noncopyable.h>

namespace AK {

template<typename...>
struct UntaggedUnion { };

template<typename T, typename... Rest>
struct UntaggedUnion<T, Rest...> {
    friend UntaggedUnion<Rest...>;

    // Allow container classes to keep their trivial default constructor.
    constexpr UntaggedUnion() = default;
    constexpr UntaggedUnion()
    requires(!(Detail::IsTriviallyConstructible<T> && (Detail::IsTriviallyConstructible<Rest> && ...)))
    {
    }

    // Allow container classes to keep their trivial destructor.
    constexpr ~UntaggedUnion() = default;
    constexpr ~UntaggedUnion()
    requires(!(Detail::IsTriviallyDestructible<T> && (Detail::IsTriviallyDestructible<Rest> && ...)))
    {
    }

    // Allow container classes to keep their trivial copy constructor.
    constexpr UntaggedUnion(UntaggedUnion const&) = default;
    constexpr UntaggedUnion(UntaggedUnion const&)
    requires(!(Detail::IsTriviallyCopyConstructible<T> && (Detail::IsTriviallyCopyConstructible<Rest> && ...)))
    {
    }
    constexpr UntaggedUnion& operator=(UntaggedUnion const&) = default;
    constexpr UntaggedUnion& operator=(UntaggedUnion const&)
    requires(!(Detail::IsTriviallyCopyConstructible<T> && (Detail::IsTriviallyCopyConstructible<Rest> && ...)))
    {
        return *this;
    }

    // Allow container classes to keep their trivial move constructor.
    constexpr UntaggedUnion(UntaggedUnion&&) = default;
    constexpr UntaggedUnion(UntaggedUnion&&)
    requires(!(Detail::IsTriviallyMoveConstructible<T> && (Detail::IsTriviallyMoveConstructible<Rest> && ...)))
    {
    }
    constexpr UntaggedUnion& operator=(UntaggedUnion&&) = default;
    constexpr UntaggedUnion& operator=(UntaggedUnion&&)
    requires(!(Detail::IsTriviallyMoveConstructible<T> && (Detail::IsTriviallyMoveConstructible<Rest> && ...)))
    {
        return *this;
    }

    // Construct an untagged union of the first element
    constexpr UntaggedUnion(T&& v)
        : value(forward<T>(v))
    {
    }

    // Construct an untagged union of one of the remaining elements
    template<typename U>
    constexpr UntaggedUnion(U&& v)
    requires(!IsSame<U, T> && (IsSame<U, Rest> || ...))
        : next(forward<U>(v))
    {
    }

    template<typename U, typename... Args>
    constexpr void set(Args&&... v)
    {
        if constexpr (IsSame<RemoveCV<T>, RemoveCV<U>>) {
            construct_at(&value, forward<Args>(v)...);
        } else {
            next.template set<U>(forward<Args>(v)...);
        }
    }

    template<typename U>
    constexpr U& get()
    {
        if constexpr (IsSame<RemoveCV<U>, T> || IsSame<RemoveVolatile<U>, T>) {
            return value;
        } else {
            return next.template get<U>();
        }
    }

    template<typename U>
    constexpr U const& get() const
    {
        if constexpr (IsSameIgnoringCV<U, T>) {
            return value;
        } else {
            return next.template get<U>();
        }
    }

    union {
        T value;
        UntaggedUnion<Rest...> next;
    };
};

}

#if USING_AK_GLOBALLY
using AK::UntaggedUnion;
#endif
