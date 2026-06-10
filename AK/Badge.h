/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>

namespace AK {

template<typename...>
class Badge;

template<typename T>
class Badge<T> {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

public:
    using Type = T;

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

private:
    friend T;
    friend U;
    constexpr Badge() = default;
};

template<typename T, typename U, typename V>
class Badge<T, U, V> {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

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
