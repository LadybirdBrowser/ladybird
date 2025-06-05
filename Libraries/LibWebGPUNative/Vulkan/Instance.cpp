/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/Error.h>
#include <LibWebGPUNative/Vulkan/InstanceImpl.h>

namespace WebGPUNative {

Instance::Instance()
    : m_impl(make<Impl>())
{
}

Instance::~Instance() = default;

ErrorOr<void> Instance::initialize()
{
    return m_impl->initialize();
}

}
