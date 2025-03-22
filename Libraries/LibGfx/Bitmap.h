/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Timothy Slater <tslater2006@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ImageOrientation.h>
#include <LibGfx/Rect.h>

namespace Gfx {

enum class BitmapFormat {
    Invalid,
    BGRx8888,
    BGRA8888,
    RGBx8888,
    RGBA8888,
};

inline bool is_valid_bitmap_format(u32 const format)
{
    switch (format) {
    case static_cast<u32>(BitmapFormat::Invalid):
    case static_cast<u32>(BitmapFormat::BGRx8888):
    case static_cast<u32>(BitmapFormat::RGBx8888):
    case static_cast<u32>(BitmapFormat::BGRA8888):
    case static_cast<u32>(BitmapFormat::RGBA8888):
        return true;
    default:
        return false;
    }
}

enum class StorageFormat {
    BGRx8888,
    BGRA8888,
    RGBA8888,
    RGBx8888,
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
    case BitmapFormat::RGBx8888:
        return StorageFormat::RGBx8888;
    default:
        VERIFY_NOT_REACHED();
    }
}

struct BackingStore;

class Bitmap : public AtomicRefCounted<Bitmap> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create(BitmapFormat, IntSize, ExifOrientation = ExifOrientation::Default);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create(BitmapFormat, AlphaType, IntSize, ExifOrientation = ExifOrientation::Default);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_shareable(BitmapFormat, AlphaType, IntSize, ExifOrientation = ExifOrientation::Default);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_wrapper(BitmapFormat, AlphaType, IntSize, size_t pitch, void*, Function<void()>&& destruction_callback = {}, ExifOrientation = ExifOrientation::Default);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_with_anonymous_buffer(BitmapFormat, AlphaType, Core::AnonymousBuffer, IntSize, ExifOrientation = ExifOrientation::Default);

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> clone() const;

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> cropped(Gfx::IntRect, Gfx::Color outside_color = Gfx::Color::Black) const;
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

    [[nodiscard]] u8* unchecked_scanline_u8(int physical_y);
    [[nodiscard]] u8 const* unchecked_scanline_u8(int physical_y) const;
    [[nodiscard]] ARGB32* unchecked_scanline(int physical_y);
    [[nodiscard]] ARGB32 const* unchecked_scanline(int physical_y) const;

    [[nodiscard]] ARGB32* begin();
    [[nodiscard]] ARGB32 const* begin() const;
    [[nodiscard]] ARGB32* end();
    [[nodiscard]] ARGB32 const* end() const;
    [[nodiscard]] size_t data_size() const;

    [[nodiscard]] IntRect rect() const;
    [[nodiscard]] IntSize size() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] ExifOrientation exif_orientation() const { return m_exif_orientation; }

    [[nodiscard]] size_t pitch() const { return m_pitch; }

    [[nodiscard]] static size_t minimum_pitch(size_t width, BitmapFormat);

    [[nodiscard]] bool has_alpha_channel() const { return m_format == BitmapFormat::BGRA8888 || m_format == BitmapFormat::RGBA8888; }
    [[nodiscard]] BitmapFormat format() const { return m_format; }

    // Call only for BGRx8888 and BGRA8888 bitmaps.
    void strip_alpha_channel();

    [[nodiscard]] static constexpr size_t size_in_bytes(size_t pitch, int height) { return pitch * height; }
    [[nodiscard]] size_t size_in_bytes() const { return size_in_bytes(m_pitch, height()); }

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

    struct DiffResult {
        bool identical { false };

        // Cumulative channel differences.
        u64 total_red_error { 0 };
        u64 total_green_error { 0 };
        u64 total_blue_error { 0 };
        u64 total_alpha_error { 0 };
        u64 total_error { 0 };

        // Maximum channel differences.
        u8 maximum_red_error { 0 };
        u8 maximum_green_error { 0 };
        u8 maximum_blue_error { 0 };
        u8 maximum_alpha_error { 0 };
        u8 maximum_error { 0 };

        // Number of pixels with errors.
        u64 pixel_error_count { 0 };
    };

    [[nodiscard]] DiffResult diff(Bitmap const&) const;

    [[nodiscard]] AlphaType alpha_type() const { return m_alpha_type; }
    void set_alpha_type_destructive(AlphaType);

private:
    Bitmap(BitmapFormat, AlphaType, IntSize, BackingStore const&, ExifOrientation);
    Bitmap(BitmapFormat, AlphaType, IntSize, size_t pitch, void*, Function<void()>&& destruction_callback, ExifOrientation);
    Bitmap(BitmapFormat, AlphaType, Core::AnonymousBuffer, IntSize, ExifOrientation);

    static ErrorOr<BackingStore> allocate_backing_store(BitmapFormat format, IntSize size);

    IntSize m_size;
    void* m_data { nullptr };
    size_t m_pitch { 0 };
    BitmapFormat m_format { BitmapFormat::Invalid };
    AlphaType m_alpha_type { AlphaType::Premultiplied };
    Core::AnonymousBuffer m_buffer;
    Function<void()> m_destruction_callback;
    ExifOrientation m_exif_orientation;
};

ALWAYS_INLINE u8* Bitmap::unchecked_scanline_u8(int y)
{
    return reinterpret_cast<u8*>(m_data) + (y * m_pitch);
}

ALWAYS_INLINE u8 const* Bitmap::unchecked_scanline_u8(int y) const
{
    return reinterpret_cast<u8 const*>(m_data) + (y * m_pitch);
}

ALWAYS_INLINE ARGB32* Bitmap::unchecked_scanline(int y)
{
    return reinterpret_cast<ARGB32*>(unchecked_scanline_u8(y));
}

ALWAYS_INLINE ARGB32 const* Bitmap::unchecked_scanline(int y) const
{
    return reinterpret_cast<ARGB32 const*>(unchecked_scanline_u8(y));
}

ALWAYS_INLINE u8* Bitmap::scanline_u8(int y)
{
    VERIFY(y >= 0);
    VERIFY(y < height());
    return unchecked_scanline_u8(y);
}

ALWAYS_INLINE u8 const* Bitmap::scanline_u8(int y) const
{
    VERIFY(y >= 0);
    VERIFY(y < height());
    return unchecked_scanline_u8(y);
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

ALWAYS_INLINE Color Bitmap::get_pixel(int x, int y) const
{
    VERIFY(x >= 0);
    VERIFY(x < width());
    auto pixel = scanline(y)[x];
    switch (determine_storage_format(m_format)) {
    case StorageFormat::BGRx8888:
        return Color::from_rgb(pixel);
    case StorageFormat::BGRA8888:
        return Color::from_argb(pixel);
    case StorageFormat::RGBA8888:
        return Color::from_abgr(pixel);
    case StorageFormat::RGBx8888:
        return Color::from_bgr(pixel);
    default:
        VERIFY_NOT_REACHED();
    }
}

template<StorageFormat storage_format>
ALWAYS_INLINE void Bitmap::set_pixel(int x, int y, Color color)
{
    VERIFY(x >= 0);
    VERIFY(x < width());

    if constexpr (storage_format == StorageFormat::BGRx8888 || storage_format == StorageFormat::BGRA8888) {
        scanline(y)[x] = color.value();
    } else if constexpr (storage_format == StorageFormat::RGBA8888) {
        scanline(y)[x] = (color.alpha() << 24) | (color.blue() << 16) | (color.green() << 8) | color.red();
    } else if constexpr (storage_format == StorageFormat::RGBx8888) {
        scanline(y)[x] = (color.blue() << 16) | (color.green() << 8) | color.red();
    } else {
        static_assert(false, "There's a new storage format not in Bitmap::set_pixel");
    }
}

ALWAYS_INLINE void Bitmap::set_pixel(int x, int y, Color color)
{
    switch (determine_storage_format(m_format)) {
    case StorageFormat::BGRx8888:
        set_pixel<StorageFormat::BGRx8888>(x, y, color);
        return;
    case StorageFormat::BGRA8888:
        set_pixel<StorageFormat::BGRA8888>(x, y, color);
        return;
    case StorageFormat::RGBA8888:
        set_pixel<StorageFormat::RGBA8888>(x, y, color);
        return;
    case StorageFormat::RGBx8888:
        set_pixel<StorageFormat::RGBx8888>(x, y, color);
        return;
    }
    VERIFY_NOT_REACHED();
}

}
