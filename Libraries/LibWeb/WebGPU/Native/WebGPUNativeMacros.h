/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>

#define WEBGPU_NATIVE_DECLARE_SPECIAL_MEMBERS(c) \
    AK_MAKE_NONCOPYABLE(c);                      \
                                                 \
public:                                          \
    c(c&&);                                      \
    c& operator=(c&&);                           \
    ~c();

#define WEBGPU_NATIVE_DEFINE_SPECIAL_MEMBERS(c) \
    c::c(c&&) = default;                        \
    c& c::operator=(c&&) = default;             \
    c::~c() = default;                          \
    c::c(Impl impl)                             \
        : m_impl(make<Impl>(move(impl)))        \
    {                                           \
    }

#define WEBGPU_NATIVE_DECLARE_PIMPL(c) \
private:                               \
    struct Impl;                       \
    c(Impl);                           \
    NonnullOwnPtr<Impl> m_impl;
