/*
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtraDetails.h>

// Enables bitwise operators for the specified Enum type.
//
#define AK_ENUM_BITWISE_OPERATORS(Enum) \
    _AK_ENUM_BITWISE_OPERATORS_INTERNAL(Enum, )

// Enables bitwise operators for the specified Enum type, this
// version is meant for use on enums which are private to the
// containing type.
//
#define AK_ENUM_BITWISE_FRIEND_OPERATORS(Enum) \
    _AK_ENUM_BITWISE_OPERATORS_INTERNAL(Enum, friend)

#define _AK_ENUM_BITWISE_OPERATORS_INTERNAL(Enum, Prefix)                  \
                                                                           \
    [[nodiscard]] Prefix constexpr Enum operator|(Enum lhs, Enum rhs)      \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        return static_cast<Enum>(                                          \
            static_cast<Type>(lhs) | static_cast<Type>(rhs));              \
    }                                                                      \
                                                                           \
    [[nodiscard]] Prefix constexpr Enum operator&(Enum lhs, Enum rhs)      \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        return static_cast<Enum>(                                          \
            static_cast<Type>(lhs) & static_cast<Type>(rhs));              \
    }                                                                      \
                                                                           \
    [[nodiscard]] Prefix constexpr Enum operator^(Enum lhs, Enum rhs)      \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        return static_cast<Enum>(                                          \
            static_cast<Type>(lhs) ^ static_cast<Type>(rhs));              \
    }                                                                      \
                                                                           \
    [[nodiscard]] Prefix constexpr Enum operator~(Enum rhs)                \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        return static_cast<Enum>(                                          \
            ~static_cast<Type>(rhs));                                      \
    }                                                                      \
                                                                           \
    Prefix constexpr Enum& operator|=(Enum& lhs, Enum rhs)                 \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        lhs = static_cast<Enum>(                                           \
            static_cast<Type>(lhs) | static_cast<Type>(rhs));              \
        return lhs;                                                        \
    }                                                                      \
                                                                           \
    Prefix constexpr Enum& operator&=(Enum& lhs, Enum rhs)                 \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        lhs = static_cast<Enum>(                                           \
            static_cast<Type>(lhs) & static_cast<Type>(rhs));              \
        return lhs;                                                        \
    }                                                                      \
                                                                           \
    Prefix constexpr Enum& operator^=(Enum& lhs, Enum rhs)                 \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        lhs = static_cast<Enum>(                                           \
            static_cast<Type>(lhs) ^ static_cast<Type>(rhs));              \
        return lhs;                                                        \
    }                                                                      \
                                                                           \
    Prefix constexpr bool has_flag(Enum value, Enum mask)                  \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        return static_cast<Type>(value & mask) == static_cast<Type>(mask); \
    }                                                                      \
                                                                           \
    Prefix constexpr bool has_any_flag(Enum value, Enum mask)              \
    {                                                                      \
        using Type = UnderlyingType<Enum>;                                 \
        return static_cast<Type>(value & mask) != 0;                       \
    }
