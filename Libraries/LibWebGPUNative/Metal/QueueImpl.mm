/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-License: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/DeviceImpl.h>
#include <LibWebGPUNative/Metal/QueueImpl.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

Queue::Impl::Impl(Device const& gpu_device)
    : m_command_queue(gpu_device.m_impl->mtl_command_queue())
{
}

}
