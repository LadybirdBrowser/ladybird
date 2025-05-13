/*
 * Copyright (c) 2022, Frhun <serenitystuff@frhun.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/StdLibExtras.h>

namespace AK {

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_one_of(T&& to_compare, Ts&&... valid_values)
{
    return (... || (forward<T>(to_compare) == forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_smaller_than_one_of(T&& to_compare, Ts&&... valid_values)
{
    return (... || (forward<T>(to_compare) < forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_smaller_or_equal_than_one_of(T&& to_compare, Ts&&... valid_values)
{
    return (... || (forward<T>(to_compare) <= forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_larger_than_one_of(T&& to_compare, Ts&&... valid_values)
{
    return (... || (forward<T>(to_compare) > forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_larger_or_equal_than_one_of(T&& to_compare, Ts&&... valid_values)
{
    return (... || (forward<T>(to_compare) >= forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_equal_to_all_of(T&& to_compare, Ts&&... valid_values)
{
    return (... && (forward<T>(to_compare) == forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_smaller_than_all_of(T&& to_compare, Ts&&... valid_values)
{
    return (... && (forward<T>(to_compare) < forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_smaller_or_equal_than_all_of(T&& to_compare, Ts&&... valid_values)
{
    return (... && (forward<T>(to_compare) <= forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_larger_than_all_of(T&& to_compare, Ts&&... valid_values)
{
    return (... && (forward<T>(to_compare) > forward<Ts>(valid_values)));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr bool first_is_larger_or_equal_than_all_of(T&& to_compare, Ts&&... valid_values)
{
    return (... && (forward<T>(to_compare) >= forward<Ts>(valid_values)));
}

}

#if USING_AK_GLOBALLY
using AK::first_is_equal_to_all_of;
using AK::first_is_larger_or_equal_than_all_of;
using AK::first_is_larger_or_equal_than_one_of;
using AK::first_is_larger_than_all_of;
using AK::first_is_larger_than_one_of;
using AK::first_is_one_of;
using AK::first_is_smaller_or_equal_than_all_of;
using AK::first_is_smaller_or_equal_than_one_of;
using AK::first_is_smaller_than_all_of;
using AK::first_is_smaller_than_one_of;
#endif
