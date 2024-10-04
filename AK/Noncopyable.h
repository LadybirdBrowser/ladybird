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
