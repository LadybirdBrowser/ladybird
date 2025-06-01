/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Span.h>
#include <LibGfx/Size.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Device;
class MappedTextureBuffer;
class Texture;

class WEBGPUNATIVE_API Texture {
public:
    friend class Device;
    friend class TextureView;
    friend class MappedTextureBuffer;

    explicit Texture(Device const&, Gfx::IntSize);
    Texture(Texture&&) noexcept;
    Texture& operator=(Texture&&) noexcept;
    ~Texture();

    ErrorOr<void> initialize();

    Gfx::IntSize size() const;

    ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> map_buffer();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

class WEBGPUNATIVE_API MappedTextureBuffer {
public:
    explicit MappedTextureBuffer(Texture::Impl& texture_impl, u8* buffer, size_t buffer_size);
    ~MappedTextureBuffer();

    Span<u8> data() const { return m_buffer; }

private:
    NonnullRawPtr<Texture::Impl> m_texture_impl;
    Span<u8> m_buffer;
};

}
