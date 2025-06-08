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

MetalTextureHandle::MetalTextureHandle(id texture)
    : m_texture(texture)
{
    [m_texture retain];
}

MetalTextureHandle::~MetalTextureHandle()
{
    if (m_texture) {
        [m_texture release];
        m_texture = nullptr;
    }
}

MetalTextureHandle::MetalTextureHandle(MetalTextureHandle&& other) noexcept
    : m_texture(other.m_texture)
{
    other.m_texture = nullptr;
}

MetalTextureHandle& MetalTextureHandle::operator=(MetalTextureHandle&& other) noexcept
{
    if (this != &other) {
        if (m_texture) {
            [m_texture release];
        }
        m_texture = other.m_texture;
        other.m_texture = nullptr;
    }
    return *this;
}

MetalBufferHandle::MetalBufferHandle(id buffer)
    : m_buffer(buffer)
{
    [m_buffer retain];
}

MetalBufferHandle::~MetalBufferHandle()
{
    if (m_buffer) {
        [m_buffer release];
        m_buffer = nullptr;
    }
}

MetalBufferHandle::MetalBufferHandle(MetalBufferHandle&& other) noexcept
    : m_buffer(other.m_buffer)
{
    other.m_buffer = nullptr;
}

MetalBufferHandle& MetalBufferHandle::operator=(MetalBufferHandle&& other) noexcept
{
    if (this != &other) {
        if (m_buffer) {
            [m_buffer release];
        }
        m_buffer = other.m_buffer;
        other.m_buffer = nullptr;
    }
    return *this;
}

MetalRenderCommandEncoderHandle::MetalRenderCommandEncoderHandle(id render_command_encoder)
    : m_render_command_encoder(render_command_encoder)
{
    [m_render_command_encoder retain];
}

MetalRenderCommandEncoderHandle::~MetalRenderCommandEncoderHandle()
{
    if (m_render_command_encoder) {
        [m_render_command_encoder release];
        m_render_command_encoder = nullptr;
    }
}

MetalRenderCommandEncoderHandle::MetalRenderCommandEncoderHandle(MetalRenderCommandEncoderHandle&& other) noexcept
    : m_render_command_encoder(other.m_render_command_encoder)
{
    other.m_render_command_encoder = nullptr;
}

MetalRenderCommandEncoderHandle& MetalRenderCommandEncoderHandle::operator=(MetalRenderCommandEncoderHandle&& other) noexcept
{
    if (this != &other) {
        if (m_render_command_encoder) {
            [m_render_command_encoder release];
        }
        m_render_command_encoder = other.m_render_command_encoder;
        other.m_render_command_encoder = nullptr;
    }
    return *this;
}

}
