/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Device.h>
#include <objc/objc.h>

namespace WebGPUNative {

struct Device::Impl {
    explicit Impl(Adapter const& gpu_adapter);
    ~Impl();

    ErrorOr<void> initialize();

    id mtl_device() const;
    id mtl_command_queue() const;

private:
    struct SwiftImpl;
    NonnullOwnPtr<SwiftImpl> m_swift_impl;
};

}
