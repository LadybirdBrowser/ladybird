/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Metal/DeviceImpl.h>

namespace WebGPUNative {

Device::Device(Adapter const& gpu_adapter)
    : m_impl(make<Impl>(gpu_adapter))
{
}

Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;
Device::~Device() = default;

ErrorOr<void> Device::initialize()
{
    return m_impl->initialize();
}

}
