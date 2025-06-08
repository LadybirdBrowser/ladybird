/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/CommandEncoder.h>
#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Queue.h>
#include <LibWebGPUNative/Texture.h>
#include <LibWebGPUNative/Vulkan/DeviceImpl.h>

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

Queue Device::queue() const
{
    return Queue(*this);
}

Texture Device::texture(Gfx::IntSize const size) const
{
    return Texture(*this, size);
}

CommandEncoder Device::command_encoder() const
{
    return CommandEncoder(*this);
}

}
