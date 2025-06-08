/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Span.h>
#include <AK/Tuple.h>
#include <LibGfx/Color.h>
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
    explicit MappedTextureBuffer(Texture::Impl& texture_impl, u8* buffer, size_t buffer_size, u32 row_pitch);
    ~MappedTextureBuffer();

    Span<u8> data() const { return m_buffer; }

    u32 row_pitch() const { return m_row_pitch; }

    int width() const;
    int height() const;

    class PixelIterator;
    struct PixelRange {
        PixelIterator begin() const;
        PixelIterator end() const;

    private:
        MappedTextureBuffer const* m_buffer;
        friend class MappedTextureBuffer;
        explicit PixelRange(MappedTextureBuffer const* buffer)
            : m_buffer(buffer)
        {
        }
    };
    PixelRange pixels() const { return PixelRange(this); }

private:
    NonnullRawPtr<Texture::Impl> m_texture_impl;
    Span<u8> m_buffer;
    u32 m_row_pitch;
};

class WEBGPUNATIVE_API MappedTextureBuffer::PixelIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Color;
    using difference_type = std::ptrdiff_t;
    using pointer = Color const*;
    using reference = Color;

    PixelIterator(MappedTextureBuffer const* buffer, int x, int y)
        : m_buffer(buffer)
        , m_x(x)
        , m_y(y)
    {
    }

    struct Pixel {
        Gfx::Color color;
        int x = 0;
        int y = 0;
    };

    Pixel operator*() const
    {
        // FIXME: Handle all supported configuration formats, not just RGBA
        size_t const offset = static_cast<size_t>(m_y) * m_buffer->row_pitch() + static_cast<size_t>(m_x) * 4;
        u8 const* data = m_buffer->data().data();

        u8 const r = data[offset + 0];
        u8 const g = data[offset + 1];
        u8 const b = data[offset + 2];
        u8 const a = data[offset + 3];
        return Pixel { .color = Gfx::Color(r, g, b, a), .x = m_x, .y = m_y };
    }

    PixelIterator& operator++()
    {
        ++m_x;
        if (m_x >= m_buffer->width()) {
            m_x = 0;
            ++m_y;
        }
        return *this;
    }

    PixelIterator operator++(int)
    {
        PixelIterator const tmp = *this;
        ++(*this);
        return tmp;
    }

    bool operator==(PixelIterator const& other) const
    {
        return m_buffer == other.m_buffer && m_x == other.m_x && m_y == other.m_y;
    }

    bool operator!=(PixelIterator const& other) const
    {
        return !(*this == other);
    }

private:
    MappedTextureBuffer const* m_buffer;
    int m_x;
    int m_y;
};

inline MappedTextureBuffer::PixelIterator MappedTextureBuffer::PixelRange::begin() const
{
    return PixelIterator(m_buffer, 0, 0);
}

inline MappedTextureBuffer::PixelIterator MappedTextureBuffer::PixelRange::end() const
{
    return PixelIterator(m_buffer, 0, m_buffer->height());
}

}
