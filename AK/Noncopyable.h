/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define AK_MAKE_NONCOPYABLE(c) \
private:                       \
    c(c const&) = delete;      \
    c& operator=(c const&) = delete

#define AK_MAKE_NONMOVABLE(c) \
private:                      \
    c(c&&) = delete;          \
    c& operator=(c&&) = delete

#define AK_MAKE_DEFAULT_MOVABLE(c) \
public:                            \
    c(c&&) = default;              \
    c& operator=(c&&) = default

#define AK_MAKE_DEFAULT_COPYABLE(c) \
public:                             \
    c(c const&) = default;          \
    c& operator=(c const&) = default

#define AK_MAKE_CONDITIONALLY_NONMOVABLE(c, ...)                                                            \
public:                                                                                                     \
    c(c&&)                                                                                                  \
    requires(!(AK::Detail::IsMoveConstructible __VA_ARGS__))                                                \
    = delete;                                                                                               \
    c& operator=(c&&)                                                                                       \
    requires(!((AK::Detail::IsMoveConstructible __VA_ARGS__) || (AK::Detail::IsMoveAssignable __VA_ARGS__)) \
                || !(AK::Detail::IsDestructible __VA_ARGS__))                                               \
    = delete

#define AK_MAKE_CONDITIONALLY_MOVABLE(c, T) \
    AK_MAKE_CONDITIONALLY_NONMOVABLE(c, T); \
    c(c&&) = default;                       \
    c& operator=(c&&) = default

#define AK_MAKE_CONDITIONALLY_NONCOPYABLE(c, ...)                                                           \
public:                                                                                                     \
    c(c const&)                                                                                             \
    requires(!(AK::Detail::IsCopyConstructible __VA_ARGS__))                                                \
    = delete;                                                                                               \
    c& operator=(c const&)                                                                                  \
    requires(!((AK::Detail::IsCopyConstructible __VA_ARGS__) || (AK::Detail::IsCopyAssignable __VA_ARGS__)) \
                || !(AK::Detail::IsDestructible __VA_ARGS__))                                               \
    = delete

#define AK_MAKE_CONDITIONALLY_COPYABLE(c, ...)         \
    AK_MAKE_CONDITIONALLY_NONCOPYABLE(c, __VA_ARGS__); \
    c(c const&) = default;                             \
    c& operator=(c const&) = default

#define AK_MAKE_CONDITIONALLY_NONDESTRUCTIBLE(c, ...)   \
public:                                                 \
    ~c()                                                \
    requires(!(AK::Detail::IsDestructible __VA_ARGS__)) \
    = delete

#define AK_MAKE_CONDITIONALLY_DESTRUCTIBLE(c, ...)      \
public:                                                 \
    ~c()                                                \
    requires(!(AK::Detail::IsDestructible __VA_ARGS__)) \
    = delete;                                           \
    ~c() = default
