/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/StdLibExtraDetails.h>

namespace AK {

namespace Detail {

template<typename AuthorizedType, typename BadgeType>
inline constexpr bool IsBadgeType = IsSame<AuthorizedType, BadgeType>
    || IsBaseOf<AuthorizedType, BadgeType>;

}

template<typename...>
class Badge;

template<typename T>
class Badge<T> {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

public:
    using Type = T;

    // Friendship is not inherited, so derived classes mint Badge<Derived> and
    // convert it to Badge<Base>.
    template<typename U>
    requires(IsBaseOf<T, U> && !IsSame<T, U>)
    constexpr Badge(Badge<U> const&)
    {
    }

private:
    friend T;
    constexpr Badge() = default;
};

// FIXME: These overloads are all because compiler support for `friend Ts...;` is limited.
//        Once compilers can do that, we should only need a single variadic template.
template<typename T, typename U>
class Badge<T, U> {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

public:
    template<typename V>
    requires(Detail::IsBadgeType<T, V> || Detail::IsBadgeType<U, V>)
    constexpr Badge(Badge<V> const&)
    {
    }

private:
    friend T;
    friend U;
    constexpr Badge() = default;
};

template<typename T, typename U, typename V>
class Badge<T, U, V> {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

public:
    template<typename W>
    requires(Detail::IsBadgeType<T, W> || Detail::IsBadgeType<U, W>
        || Detail::IsBadgeType<V, W>)
    constexpr Badge(Badge<W> const&)
    {
    }

private:
    friend T;
    friend U;
    friend V;
    constexpr Badge() = default;
};

template<typename T, typename U, typename V, typename W>
class Badge<T, U, V, W> {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

public:
    template<typename X>
    requires(Detail::IsBadgeType<T, X> || Detail::IsBadgeType<U, X>
        || Detail::IsBadgeType<V, X> || Detail::IsBadgeType<W, X>)
    constexpr Badge(Badge<X> const&)
    {
    }

private:
    friend T;
    friend U;
    friend V;
    friend W;
    constexpr Badge() = default;
};

template<typename T, typename U, typename V, typename W, typename X>
class Badge<T, U, V, W, X> {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

public:
    template<typename Y>
    requires(Detail::IsBadgeType<T, Y> || Detail::IsBadgeType<U, Y>
        || Detail::IsBadgeType<V, Y> || Detail::IsBadgeType<W, Y>
        || Detail::IsBadgeType<X, Y>)
    constexpr Badge(Badge<Y> const&)
    {
    }

private:
    friend T;
    friend U;
    friend V;
    friend W;
    friend X;
    constexpr Badge() = default;
};

}

#if USING_AK_GLOBALLY
using AK::Badge;
#endif
