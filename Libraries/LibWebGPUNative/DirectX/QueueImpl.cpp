/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/DirectX/QueueImpl.h>

namespace WebGPUNative {

Queue::Impl::Impl(Device const& gpu_device)
    : m_command_queue(gpu_device.m_impl->command_queue())

{
}

}
