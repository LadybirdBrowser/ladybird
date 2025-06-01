/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Adapter.h>
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

Adapter Instance::adapter() const
{
    return Adapter(*this);
}

NonnullRefPtr<Core::Promise<Adapter>> Instance::request_adapter()
{
    return MUST(Core::Promise<Adapter>::try_create());
}

}
