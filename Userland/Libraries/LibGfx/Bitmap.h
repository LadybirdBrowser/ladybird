/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Timothy Slater <tslater2006@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace Gfx {

enum class BitmapFormat {
    Invalid,
    BGRx8888,
    BGRA8888,
    RGBA8888,
};

inline bool is_valid_bitmap_format(unsigned format)
{
    switch (format) {
    case (unsigned)BitmapFormat::Invalid:
    case (unsigned)BitmapFormat::BGRx8888:
    case (unsigned)BitmapFormat::BGRA8888:
    case (unsigned)BitmapFormat::RGBA8888:
        return true;
    }
    return false;
}

enum class StorageFormat {
    BGRx8888,
    BGRA8888,
    RGBA8888,
};

inline StorageFormat determine_storage_format(BitmapFormat format)
{
    switch (format) {
    case BitmapFormat::BGRx8888:
        return StorageFormat::BGRx8888;
    case BitmapFormat::BGRA8888:
        return StorageFormat::BGRA8888;
    case BitmapFormat::RGBA8888:
        return StorageFormat::RGBA8888;
    default:
        VERIFY_NOT_REACHED();
    }
}

struct BackingStore;

class Bitmap : public RefCounted<Bitmap> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create(BitmapFormat, IntSize);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_shareable(BitmapFormat, IntSize);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_wrapper(BitmapFormat, IntSize, size_t pitch, void*);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> load_from_file(StringView path, Optional<IntSize> ideal_size = {});
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> load_from_file(NonnullOwnPtr<Core::File>, StringView path, Optional<IntSize> ideal_size = {});
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> load_from_bytes(ReadonlyBytes, Optional<IntSize> ideal_size = {}, Optional<ByteString> mine_type = {});
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_with_anonymous_buffer(BitmapFormat, Core::AnonymousBuffer, IntSize);

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> clone() const;

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> scaled(int sx, int sy) const;
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> scaled(float sx, float sy) const;
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> scaled_to_size(Gfx::IntSize) const;
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> cropped(Gfx::IntRect, Optional<BitmapFormat> new_bitmap_format = {}) const;
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> to_bitmap_backed_by_anonymous_buffer() const;

    [[nodiscard]] ShareableBitmap to_shareable_bitmap() const;

    enum class MaskKind {
        Alpha,
        Luminance
    };

    void apply_mask(Gfx::Bitmap const& mask, MaskKind);

    ~Bitmap();

    [[nodiscard]] u8* scanline_u8(int physical_y);
    [[nodiscard]] u8 const* scanline_u8(int physical_y) const;
    [[nodiscard]] ARGB32* scanline(int physical_y);
    [[nodiscard]] ARGB32 const* scanline(int physical_y) const;

    [[nodiscard]] ARGB32* begin();
    [[nodiscard]] ARGB32 const* begin() const;
    [[nodiscard]] ARGB32* end();
    [[nodiscard]] ARGB32 const* end() const;
    [[nodiscard]] size_t data_size() const;

    [[nodiscard]] IntRect rect() const { return { {}, m_size }; }
    [[nodiscard]] IntSize size() const { return m_size; }
    [[nodiscard]] int width() const { return m_size.width(); }
    [[nodiscard]] int height() const { return m_size.height(); }

    [[nodiscard]] size_t pitch() const { return m_pitch; }

    [[nodiscard]] static unsigned bpp_for_format(BitmapFormat format)
    {
        switch (format) {
        case BitmapFormat::BGRx8888:
        case BitmapFormat::BGRA8888:
            return 32;
        default:
            VERIFY_NOT_REACHED();
        case BitmapFormat::Invalid:
            return 0;
        }
    }

    [[nodiscard]] static size_t minimum_pitch(size_t width, BitmapFormat);

    [[nodiscard]] unsigned bpp() const
    {
        return bpp_for_format(m_format);
    }

    void fill(Color);

    [[nodiscard]] bool has_alpha_channel() const { return m_format == BitmapFormat::BGRA8888 || m_format == BitmapFormat::RGBA8888; }
    [[nodiscard]] BitmapFormat format() const { return m_format; }

    // Call only for BGRx8888 and BGRA8888 bitmaps.
    void strip_alpha_channel();

    [[nodiscard]] static constexpr size_t size_in_bytes(size_t pitch, int height) { return pitch * height; }
    [[nodiscard]] size_t size_in_bytes() const { return size_in_bytes(m_pitch, height()); }

    template<StorageFormat>
    [[nodiscard]] Color get_pixel(int physical_x, int physical_y) const;
    [[nodiscard]] Color get_pixel(int physical_x, int physical_y) const;
    [[nodiscard]] Color get_pixel(IntPoint physical_position) const
    {
        return get_pixel(physical_position.x(), physical_position.y());
    }

    template<StorageFormat>
    void set_pixel(int physical_x, int physical_y, Color);
    void set_pixel(int physical_x, int physical_y, Color);
    void set_pixel(IntPoint physical_position, Color color)
    {
        set_pixel(physical_position.x(), physical_position.y(), color);
    }

    [[nodiscard]] Core::AnonymousBuffer& anonymous_buffer() { return m_buffer; }
    [[nodiscard]] Core::AnonymousBuffer const& anonymous_buffer() const { return m_buffer; }

    [[nodiscard]] bool visually_equals(Bitmap const&) const;

private:
    Bitmap(BitmapFormat, IntSize, BackingStore const&);
    Bitmap(BitmapFormat, IntSize, size_t pitch, void*);
    Bitmap(BitmapFormat, Core::AnonymousBuffer, IntSize);

    static ErrorOr<BackingStore> allocate_backing_store(BitmapFormat format, IntSize size);

    IntSize m_size;
    void* m_data { nullptr };
    size_t m_pitch { 0 };
    BitmapFormat m_format { BitmapFormat::Invalid };
    bool m_data_is_malloced { false };
    Core::AnonymousBuffer m_buffer;
};

ALWAYS_INLINE u8* Bitmap::scanline_u8(int y)
{
    VERIFY(y >= 0);
    VERIFY(y < height());
    return reinterpret_cast<u8*>(m_data) + (y * m_pitch);
}

ALWAYS_INLINE u8 const* Bitmap::scanline_u8(int y) const
{
    VERIFY(y >= 0);
    VERIFY(y < height());
    return reinterpret_cast<u8 const*>(m_data) + (y * m_pitch);
}

ALWAYS_INLINE ARGB32* Bitmap::scanline(int y)
{
    return reinterpret_cast<ARGB32*>(scanline_u8(y));
}

ALWAYS_INLINE ARGB32 const* Bitmap::scanline(int y) const
{
    return reinterpret_cast<ARGB32 const*>(scanline_u8(y));
}

ALWAYS_INLINE ARGB32* Bitmap::begin()
{
    return scanline(0);
}

ALWAYS_INLINE ARGB32 const* Bitmap::begin() const
{
    return scanline(0);
}

ALWAYS_INLINE ARGB32* Bitmap::end()
{
    return reinterpret_cast<ARGB32*>(reinterpret_cast<u8*>(m_data) + data_size());
}

ALWAYS_INLINE ARGB32 const* Bitmap::end() const
{
    return reinterpret_cast<ARGB32 const*>(reinterpret_cast<u8 const*>(m_data) + data_size());
}

ALWAYS_INLINE size_t Bitmap::data_size() const
{
    return m_size.height() * m_pitch;
}

template<>
ALWAYS_INLINE Color Bitmap::get_pixel<StorageFormat::BGRx8888>(int x, int y) const
{
    VERIFY(x >= 0);
    VERIFY(x < width());
    return Color::from_rgb(scanline(y)[x]);
}

template<>
ALWAYS_INLINE Color Bitmap::get_pixel<StorageFormat::BGRA8888>(int x, int y) const
{
    VERIFY(x >= 0);
    VERIFY(x < width());
    return Color::from_argb(scanline(y)[x]);
}

ALWAYS_INLINE Color Bitmap::get_pixel(int x, int y) const
{
    switch (determine_storage_format(m_format)) {
    case StorageFormat::BGRx8888:
        return get_pixel<StorageFormat::BGRx8888>(x, y);
    case StorageFormat::BGRA8888:
        return get_pixel<StorageFormat::BGRA8888>(x, y);
    default:
        VERIFY_NOT_REACHED();
    }
}

template<>
ALWAYS_INLINE void Bitmap::set_pixel<StorageFormat::BGRx8888>(int x, int y, Color color)
{
    VERIFY(x >= 0);
    VERIFY(x < width());
    scanline(y)[x] = color.value();
}

template<>
ALWAYS_INLINE void Bitmap::set_pixel<StorageFormat::BGRA8888>(int x, int y, Color color)
{
    VERIFY(x >= 0);
    VERIFY(x < width());
    scanline(y)[x] = color.value(); // drop alpha
}

template<>
ALWAYS_INLINE void Bitmap::set_pixel<StorageFormat::RGBA8888>(int x, int y, Color color)
{
    VERIFY(x >= 0);
    VERIFY(x < width());
    // FIXME: There's a lot of inaccurately named functions in the Color class right now (RGBA vs BGRA),
    //        clear those up and then make this more convenient.
    auto rgba = (color.alpha() << 24) | (color.blue() << 16) | (color.green() << 8) | color.red();
    scanline(y)[x] = rgba;
}

ALWAYS_INLINE void Bitmap::set_pixel(int x, int y, Color color)
{
    switch (determine_storage_format(m_format)) {
    case StorageFormat::BGRx8888:
        set_pixel<StorageFormat::BGRx8888>(x, y, color);
        break;
    case StorageFormat::BGRA8888:
        set_pixel<StorageFormat::BGRA8888>(x, y, color);
        break;
    case StorageFormat::RGBA8888:
        set_pixel<StorageFormat::RGBA8888>(x, y, color);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

}
