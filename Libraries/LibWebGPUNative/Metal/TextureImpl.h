/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebGPUNative/Metal/Handle.h>
#include <LibWebGPUNative/Texture.h>

namespace WebGPUNative {

struct Texture::Impl {
    explicit Impl(Device const& gpu_device, Gfx::IntSize size);
    ~Impl() = default;

    ErrorOr<void> initialize();

    Gfx::IntSize size() const { return m_size; }
    id texture() const { return m_texture->get(); }

    ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> map_buffer();
    void unmap_buffer();

private:
    Gfx::IntSize m_size;
    OwnPtr<MetalCommandQueueHandle> m_command_queue;
    OwnPtr<MetalTextureHandle> m_texture;
};

}
