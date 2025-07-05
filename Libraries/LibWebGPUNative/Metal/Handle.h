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

class MetalTextureHandle {
public:
    explicit MetalTextureHandle(id texture);
    ~MetalTextureHandle();

    MetalTextureHandle(MetalTextureHandle const&) = delete;
    MetalTextureHandle& operator=(MetalTextureHandle const&) = delete;
    MetalTextureHandle(MetalTextureHandle&&) noexcept;
    MetalTextureHandle& operator=(MetalTextureHandle&&) noexcept;

    id get() const { return m_texture; }

private:
    id m_texture;
};

class MetalBufferHandle {
public:
    explicit MetalBufferHandle(id buffer);
    ~MetalBufferHandle();

    MetalBufferHandle(MetalBufferHandle const&) = delete;
    MetalBufferHandle& operator=(MetalBufferHandle const&) = delete;
    MetalBufferHandle(MetalBufferHandle&&) noexcept;
    MetalBufferHandle& operator=(MetalBufferHandle&&) noexcept;

    id get() const { return m_buffer; }

private:
    id m_buffer;
};

class MetalRenderCommandEncoderHandle {
public:
    explicit MetalRenderCommandEncoderHandle(id render_command_encoder);
    ~MetalRenderCommandEncoderHandle();

    MetalRenderCommandEncoderHandle(MetalRenderCommandEncoderHandle const&) = delete;
    MetalRenderCommandEncoderHandle& operator=(MetalRenderCommandEncoderHandle const&) = delete;
    MetalRenderCommandEncoderHandle(MetalRenderCommandEncoderHandle&&) noexcept;
    MetalRenderCommandEncoderHandle& operator=(MetalRenderCommandEncoderHandle&&) noexcept;

    id get() const { return m_render_command_encoder; }

private:
    id m_render_command_encoder;
};

}
