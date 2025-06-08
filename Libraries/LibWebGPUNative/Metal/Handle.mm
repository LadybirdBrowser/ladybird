/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/Handle.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

MetalDeviceHandle::MetalDeviceHandle(id device)
    : m_device(device)
{
    [m_device retain];
}

MetalDeviceHandle::~MetalDeviceHandle()
{
    if (m_device) {
        [m_device release];
        m_device = nullptr;
    }
}

MetalDeviceHandle::MetalDeviceHandle(MetalDeviceHandle&& other) noexcept
    : m_device(other.m_device)
{
    other.m_device = nullptr;
}

MetalDeviceHandle& MetalDeviceHandle::operator=(MetalDeviceHandle&& other) noexcept
{
    if (this != &other) {
        if (m_device) {
            [m_device release];
        }
        m_device = other.m_device;
        other.m_device = nullptr;
    }
    return *this;
}

MetalCommandQueueHandle::MetalCommandQueueHandle(id command_queue)
    : m_command_queue(command_queue)
{
    [m_command_queue retain];
}

MetalCommandQueueHandle::~MetalCommandQueueHandle()
{
    if (m_command_queue) {
        [m_command_queue release];
        m_command_queue = nullptr;
    }
}

MetalCommandQueueHandle::MetalCommandQueueHandle(MetalCommandQueueHandle&& other) noexcept
    : m_command_queue(other.m_command_queue)
{
    other.m_command_queue = nullptr;
}

MetalCommandQueueHandle& MetalCommandQueueHandle::operator=(MetalCommandQueueHandle&& other) noexcept
{
    if (this != &other) {
        if (m_command_queue) {
            [m_command_queue release];
        }
        m_command_queue = other.m_command_queue;
        other.m_command_queue = nullptr;
    }
    return *this;
}

MetalCommandBufferHandle::MetalCommandBufferHandle(id command_buffer)
    : m_command_buffer(command_buffer)
{
    [m_command_buffer retain];
}

MetalCommandBufferHandle::~MetalCommandBufferHandle()
{
    if (m_command_buffer) {
        [m_command_buffer release];
        m_command_buffer = nullptr;
    }
}

MetalCommandBufferHandle::MetalCommandBufferHandle(MetalCommandBufferHandle&& other) noexcept
    : m_command_buffer(other.m_command_buffer)
{
    other.m_command_buffer = nullptr;
}

MetalCommandBufferHandle& MetalCommandBufferHandle::operator=(MetalCommandBufferHandle&& other) noexcept
{
    if (this != &other) {
        if (m_command_buffer) {
            [m_command_buffer release];
        }
        m_command_buffer = other.m_command_buffer;
        other.m_command_buffer = nullptr;
    }
    return *this;
}

}
