/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Adapter.h>
#include <objc/objc.h>

namespace WebGPUNative {

struct Adapter::Impl {
    explicit Impl(Instance const&);
    ~Impl();

    ErrorOr<void> initialize();

    id mtl_device() const;

private:
    struct SwiftImpl;
    NonnullOwnPtr<SwiftImpl> m_swift_impl;
};

}
