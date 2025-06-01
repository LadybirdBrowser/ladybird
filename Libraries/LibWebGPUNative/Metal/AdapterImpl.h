/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Adapter.h>
#include <objc/objc.h>

namespace WebGPUNative {

class AdapterImplBridge {
public:
    explicit AdapterImplBridge();
    ~AdapterImplBridge();

    bool initialize();
    id metal_device();

private:
    void* m_metal_device;
};

struct Adapter::Impl {
    explicit Impl(Instance const&) { }

    ErrorOr<void> initialize();

    id metal_device() const
    {
        return m_metal_device;
    }

private:
    AdapterImplBridge adapter_bridge;
    id m_metal_device = nullptr;
};

}
