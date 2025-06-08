/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <objc/objc.h>

namespace WebGPUNative {

class MetalDeviceHandle {
public:
    explicit MetalDeviceHandle(id device);
    ~MetalDeviceHandle();

    MetalDeviceHandle(MetalDeviceHandle const&) = delete;
    MetalDeviceHandle& operator=(MetalDeviceHandle const&) = delete;
    MetalDeviceHandle(MetalDeviceHandle&&) noexcept;
    MetalDeviceHandle& operator=(MetalDeviceHandle&&) noexcept;

    id get() const { return m_device; }

private:
    id m_device;
};

class MetalCommandQueueHandle {
public:
    explicit MetalCommandQueueHandle(id command_queue);
    ~MetalCommandQueueHandle();

    MetalCommandQueueHandle(MetalCommandQueueHandle const&) = delete;
    MetalCommandQueueHandle& operator=(MetalCommandQueueHandle const&) = delete;
    MetalCommandQueueHandle(MetalCommandQueueHandle&&) noexcept;
    MetalCommandQueueHandle& operator=(MetalCommandQueueHandle&&) noexcept;

    id get() const { return m_command_queue; }

private:
    id m_command_queue;
};

class MetalCommandBufferHandle {
public:
    explicit MetalCommandBufferHandle(id command_buffer);
    ~MetalCommandBufferHandle();

    MetalCommandBufferHandle(MetalCommandBufferHandle const&) = delete;
    MetalCommandBufferHandle& operator=(MetalCommandBufferHandle const&) = delete;
    MetalCommandBufferHandle(MetalCommandBufferHandle&&) noexcept;
    MetalCommandBufferHandle& operator=(MetalCommandBufferHandle&&) noexcept;

    id get() const { return m_command_buffer; }

private:
    id m_command_buffer;
};

}
