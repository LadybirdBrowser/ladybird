/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
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
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>

namespace Gfx {

// A pixel value that does not express any information about its component order
using RawPixel = u32;

#define ENUMERATE_BITMAP_FORMATS(X) \
    X(Invalid)                      \
    X(BGRx8888)                     \
    X(BGRA8888)                     \
    X(RGBx8888)                     \
    X(RGBA8888)

enum class BitmapFormat {
#define ENUMERATE_BITMAP_FORMAT(format) format,
    ENUMERATE_BITMAP_FORMATS(ENUMERATE_BITMAP_FORMAT)
#undef ENUMERATE_BITMAP_FORMAT
};

[[nodiscard]] StringView bitmap_format_name(BitmapFormat);

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

struct BackingStore;

class Bitmap : public AtomicRefCounted<Bitmap> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create(BitmapFormat, IntSize);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create(BitmapFormat, AlphaType, IntSize);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_shareable(BitmapFormat, AlphaType, IntSize);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_wrapper(BitmapFormat, AlphaType, IntSize, size_t pitch, void*, Function<void()>&& destruction_callback = {});
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_with_raw_data(BitmapFormat, AlphaType, ReadonlyBytes, IntSize);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create_with_anonymous_buffer(BitmapFormat, AlphaType, Core::AnonymousBuffer, IntSize);

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> clone() const;

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> cropped(Gfx::IntRect, Gfx::Color outside_color = Gfx::Color::Black) const;
    ErrorOr<NonnullRefPtr<Bitmap>> scaled(int width, int height, ScalingMode scaling_mode) const;

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> to_bitmap_backed_by_anonymous_buffer() const;

    [[nodiscard]] ShareableBitmap to_shareable_bitmap() const;

    ~Bitmap();

    [[nodiscard]] u8* scanline_u8(int physical_y);
    [[nodiscard]] u8 const* scanline_u8(int physical_y) const;
    [[nodiscard]] RawPixel* scanline(int physical_y);
    [[nodiscard]] RawPixel const* scanline(int physical_y) const;

    [[nodiscard]] u8* unchecked_scanline_u8(int physical_y);
    [[nodiscard]] u8 const* unchecked_scanline_u8(int physical_y) const;
    [[nodiscard]] RawPixel* unchecked_scanline(int physical_y);
    [[nodiscard]] RawPixel const* unchecked_scanline(int physical_y) const;

    [[nodiscard]] RawPixel* begin();
    [[nodiscard]] RawPixel const* begin() const;
    [[nodiscard]] RawPixel* end();
    [[nodiscard]] RawPixel const* end() const;
    [[nodiscard]] size_t data_size() const;

    [[nodiscard]] IntRect rect() const { return { {}, m_size }; }
    [[nodiscard]] IntSize size() const { return m_size; }
    [[nodiscard]] int width() const { return m_size.width(); }
    [[nodiscard]] int height() const { return m_size.height(); }

    [[nodiscard]] size_t pitch() const { return m_pitch; }

    [[nodiscard]] static size_t minimum_pitch(size_t width, BitmapFormat);

    [[nodiscard]] bool has_alpha_channel() const { return m_format == BitmapFormat::BGRA8888 || m_format == BitmapFormat::RGBA8888; }
    [[nodiscard]] BitmapFormat format() const { return m_format; }

    // Call only for BGRx8888 and BGRA8888 bitmaps.
    void strip_alpha_channel();

    [[nodiscard]] static constexpr size_t size_in_bytes(size_t pitch, int height) { return pitch * height; }
    [[nodiscard]] size_t size_in_bytes() const { return size_in_bytes(m_pitch, height()); }

    [[nodiscard]] Color get_pixel(int physical_x, int physical_y) const;
    void set_pixel(int physical_x, int physical_y, Color);

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
    Bitmap(BitmapFormat, AlphaType, IntSize, BackingStore const&);
    Bitmap(BitmapFormat, AlphaType, IntSize, size_t pitch, void*, Function<void()>&& destruction_callback);
    Bitmap(BitmapFormat, AlphaType, Core::AnonymousBuffer, IntSize);

    enum class InitializeBackingStore {
        No,
        Yes,
    };
    static ErrorOr<BackingStore> allocate_backing_store(BitmapFormat format, IntSize size, InitializeBackingStore = InitializeBackingStore::Yes);

    IntSize m_size;
    void* m_data { nullptr };
    size_t m_pitch { 0 };
    BitmapFormat m_format { BitmapFormat::Invalid };
    AlphaType m_alpha_type { AlphaType::Premultiplied };
    Core::AnonymousBuffer m_buffer;
    Function<void()> m_destruction_callback;
};

ALWAYS_INLINE u8* Bitmap::unchecked_scanline_u8(int y)
{
    return reinterpret_cast<u8*>(m_data) + (y * m_pitch);
}

ALWAYS_INLINE u8 const* Bitmap::unchecked_scanline_u8(int y) const
{
    return reinterpret_cast<u8 const*>(m_data) + (y * m_pitch);
}

ALWAYS_INLINE RawPixel* Bitmap::unchecked_scanline(int y)
{
    return reinterpret_cast<RawPixel*>(unchecked_scanline_u8(y));
}

ALWAYS_INLINE RawPixel const* Bitmap::unchecked_scanline(int y) const
{
    return reinterpret_cast<RawPixel const*>(unchecked_scanline_u8(y));
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

ALWAYS_INLINE RawPixel* Bitmap::scanline(int y)
{
    return reinterpret_cast<RawPixel*>(scanline_u8(y));
}

ALWAYS_INLINE RawPixel const* Bitmap::scanline(int y) const
{
    return reinterpret_cast<RawPixel const*>(scanline_u8(y));
}

ALWAYS_INLINE RawPixel* Bitmap::begin()
{
    return scanline(0);
}

ALWAYS_INLINE RawPixel const* Bitmap::begin() const
{
    return scanline(0);
}

ALWAYS_INLINE RawPixel* Bitmap::end()
{
    return reinterpret_cast<RawPixel*>(reinterpret_cast<u8*>(m_data) + data_size());
}

ALWAYS_INLINE RawPixel const* Bitmap::end() const
{
    return reinterpret_cast<RawPixel const*>(reinterpret_cast<u8 const*>(m_data) + data_size());
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
    switch (m_format) {
    case BitmapFormat::BGRx8888:
        return Color::from_bgrx(pixel);
    case BitmapFormat::BGRA8888:
        return Color::from_bgra(pixel);
    case BitmapFormat::RGBA8888:
        return Color::from_rgba(pixel);
    case BitmapFormat::RGBx8888:
        return Color::from_rgbx(pixel);
    case BitmapFormat::Invalid:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

ALWAYS_INLINE void Bitmap::set_pixel(int x, int y, Color color)
{
    switch (m_format) {
    case BitmapFormat::BGRA8888:
        scanline(y)[x] = color.value();
        return;
    case BitmapFormat::BGRx8888:
        scanline(y)[x] = color.value() | (0xFF << 24);
        return;
    case BitmapFormat::RGBA8888:
        scanline(y)[x] = (color.alpha() << 24) | (color.blue() << 16) | (color.green() << 8) | color.red();
        return;
    case BitmapFormat::RGBx8888:
        scanline(y)[x] = (0xFF << 24) | (color.blue() << 16) | (color.green() << 8) | color.red();
        return;
    case BitmapFormat::Invalid:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

}
