/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/TextureImpl.h>
#include <LibWebGPUNative/Texture.h>
#include <LibWebGPUNative/TextureView.h>

namespace WebGPUNative {

Texture::Texture(Device const& gpu_device, Gfx::IntSize size)
    : m_impl(make<Impl>(gpu_device, size))
{
}

Texture::Texture(Texture&&) noexcept = default;
Texture& Texture::operator=(Texture&&) noexcept = default;
Texture::~Texture() = default;

ErrorOr<void> Texture::initialize()
{
    return m_impl->initialize();
}

Gfx::IntSize Texture::size() const
{
    return m_impl->size();
}

ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> Texture::map_buffer()
{
    return m_impl->map_buffer();
}

TextureView Texture::texture_view() const
{
    return TextureView(*this);
}

}
