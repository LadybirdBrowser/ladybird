/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Timothy Slater <tslater2006@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <AK/Checked.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>

#include <errno.h>

#ifdef AK_OS_MACOS
#    include <Accelerate/Accelerate.h>
#endif

namespace Gfx {

struct BackingStore {
    void* data { nullptr };
    size_t pitch { 0 };
    size_t size_in_bytes { 0 };
};

StringView bitmap_format_name(BitmapFormat format)
{
    switch (format) {
#define ENUMERATE_BITMAP_FORMAT(format) \
    case BitmapFormat::format:          \
        return #format##sv;
        ENUMERATE_BITMAP_FORMATS(ENUMERATE_BITMAP_FORMAT)
#undef ENUMERATE_BITMAP_FORMAT
    }
    VERIFY_NOT_REACHED();
}

size_t Bitmap::minimum_pitch(size_t width, BitmapFormat format)
{
    size_t element_size;
    switch (format) {
    case BitmapFormat::BGRx8888:
    case BitmapFormat::BGRA8888:
    case BitmapFormat::RGBx8888:
    case BitmapFormat::RGBA8888:
        element_size = 4;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    return width * element_size;
}

bool Bitmap::size_would_overflow(BitmapFormat format, IntSize size)
{
    if (size.width() < 0 || size.height() < 0)
        return true;

    if (size.width() > UINT16_MAX || size.height() > UINT16_MAX)
        return true;

    size_t pitch = Bitmap::minimum_pitch(size.width(), format);
    return Checked<int32_t>::multiplication_would_overflow(pitch, size.height());
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create(BitmapFormat format, IntSize size)
{
    // For backwards compatibility, premultiplied alpha is assumed
    return create(format, AlphaType::Premultiplied, size);
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create(BitmapFormat format, AlphaType alpha_type, IntSize size)
{
    auto backing_store = TRY(Bitmap::allocate_backing_store(format, size));
    return AK::adopt_nonnull_ref_or_enomem(new (nothrow) Bitmap(format, alpha_type, size, backing_store));
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_shareable(BitmapFormat format, AlphaType alpha_type, IntSize size)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_shareable size overflow");

    auto const pitch = minimum_pitch(size.width(), format);
    auto const data_size = size_in_bytes(pitch, size.height());

    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(round_up_to_power_of_two(data_size, PAGE_SIZE)));
    auto bitmap = TRY(Bitmap::create_with_anonymous_buffer(format, alpha_type, buffer, size));
    return bitmap;
}

#ifdef AK_OS_MACOS
static u32 iosurface_pixel_format_of(BitmapFormat format)
{
    switch (format) {
    case BitmapFormat::BGRx8888:
    case BitmapFormat::BGRA8888:
        return 'BGRA';
    case BitmapFormat::RGBx8888:
    case BitmapFormat::RGBA8888:
        return 'RGBA';
    case BitmapFormat::Invalid:
        break;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> Bitmap::validate_iosurface_holds_pixels_of(Core::IOSurfaceHandle const& iosurface, BitmapFormat format, IntSize size)
{
    if (!is_valid_bitmap_format(to_underlying(format)))
        return Error::from_string_literal("Gfx::Bitmap IOSurface format is invalid");
    if (size.is_empty() || size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap IOSurface size is invalid");
    if (iosurface.plane_count() != 0 || iosurface.bytes_per_element() != 4 || iosurface.pixel_format() != iosurface_pixel_format_of(format))
        return Error::from_string_literal("Gfx::Bitmap IOSurface does not hold pixels of its format");
    if (iosurface.width() != static_cast<size_t>(size.width()) || iosurface.height() != static_cast<size_t>(size.height()))
        return Error::from_string_literal("Gfx::Bitmap IOSurface is not of its size");

    auto bytes_per_row = iosurface.bytes_per_row();
    if (bytes_per_row < minimum_pitch(size.width(), format) || bytes_per_row % alignof(RawPixel) != 0)
        return Error::from_string_literal("Gfx::Bitmap IOSurface rows are invalid");
    Checked<size_t> size_of_pixels = bytes_per_row;
    size_of_pixels *= static_cast<size_t>(size.height());
    if (size_of_pixels.has_overflow() || size_of_pixels.value() > iosurface.alloc_size())
        return Error::from_string_literal("Gfx::Bitmap IOSurface rows are outside of its memory");
    if (!iosurface.data())
        return Error::from_string_literal("Gfx::Bitmap IOSurface is not mapped");
    return {};
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_with_iosurface(BitmapFormat format, AlphaType alpha_type, IntSize size, Core::IOSurfaceHandle iosurface)
{
    TRY(validate_iosurface_holds_pixels_of(iosurface, format, size));
    return adopt_nonnull_ref_or_enomem(new (nothrow) Bitmap(format, alpha_type, size, move(iosurface)));
}

Bitmap::Bitmap(BitmapFormat format, AlphaType alpha_type, IntSize size, Core::IOSurfaceHandle iosurface)
    : m_size(size)
    , m_data(iosurface.data())
    , m_pitch(iosurface.bytes_per_row())
    , m_format(format)
    , m_alpha_type(alpha_type)
    , m_iosurface(move(iosurface))
{
}

static ErrorOr<NonnullRefPtr<Bitmap>> create_over_new_iosurface(BitmapFormat format, AlphaType alpha_type, IntSize size)
{
    if (size.is_empty() || Bitmap::size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap IOSurface size is invalid");

    // NB: The rows of a surface only a few pixels wide are mostly padding, which would take more memory than shared
    //     memory and a copy of it on the GPU take together.
    auto minimum_pitch = Bitmap::minimum_pitch(size.width(), format);
    if (Core::IOSurfaceHandle::bytes_per_row_aligned_as_the_system_aligns_it(minimum_pitch) > 2 * minimum_pitch)
        return Error::from_string_literal("Gfx::Bitmap IOSurface rows would be mostly padding");

    auto iosurface = TRY(Core::IOSurfaceHandle::try_create_with_four_bytes_per_pixel(size.width(), size.height(), iosurface_pixel_format_of(format)));
    return Bitmap::create_with_iosurface(format, alpha_type, size, move(iosurface));
}
#endif

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_shareable_with_gpu_access(BitmapFormat format, AlphaType alpha_type, IntSize size)
{
#ifdef AK_OS_MACOS
    if (auto bitmap = create_over_new_iosurface(format, alpha_type, size); !bitmap.is_error())
        return bitmap.release_value();
#endif
    return create_shareable(format, alpha_type, size);
}

Bitmap::Bitmap(BitmapFormat format, AlphaType alpha_type, IntSize size, BackingStore const& backing_store)
    : m_size(size)
    , m_data(backing_store.data)
    , m_pitch(backing_store.pitch)
    , m_format(format)
    , m_alpha_type(alpha_type)
{
    VERIFY(!m_size.is_empty());
    VERIFY(!size_would_overflow(format, size));
    VERIFY(m_data);
    VERIFY(backing_store.size_in_bytes == size_in_bytes());
    m_destruction_callback = [data = m_data] {
        kfree(data);
    };
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_wrapper(BitmapFormat format, AlphaType alpha_type, IntSize size, size_t pitch, void* data, Function<void()>&& destruction_callback)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_wrapper size overflow");
    return adopt_ref(*new Bitmap(format, alpha_type, size, pitch, data, move(destruction_callback)));
}

Bitmap::Bitmap(BitmapFormat format, AlphaType alpha_type, IntSize size, size_t pitch, void* data, Function<void()>&& destruction_callback)
    : m_size(size)
    , m_data(data)
    , m_pitch(pitch)
    , m_format(format)
    , m_alpha_type(alpha_type)
    , m_destruction_callback(move(destruction_callback))
{
    VERIFY(pitch >= minimum_pitch(size.width(), format));
    VERIFY(!size_would_overflow(format, size));
    // FIXME: assert that `data` is actually long enough!
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_with_anonymous_buffer(BitmapFormat format, AlphaType alpha_type, Core::AnonymousBuffer buffer, IntSize size)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_with_anonymous_buffer size overflow");

    if (buffer.size() < size_in_bytes(minimum_pitch(size.width(), format), size.height()))
        return Error::from_string_literal("Gfx::Bitmap::create_with_anonymous_buffer buffer too small for size");

    return adopt_nonnull_ref_or_enomem(new (nothrow) Bitmap(format, alpha_type, move(buffer), size));
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_with_raw_data(BitmapFormat format, AlphaType alpha_type, ReadonlyBytes raw_data, IntSize size)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_with_raw_data size overflow");

    if (raw_data.size() < size_in_bytes(minimum_pitch(size.width(), format), size.height()))
        return Error::from_string_literal("Gfx::Bitmap::create_with_raw_data data too small for size");

    auto backing_store = TRY(Bitmap::allocate_backing_store(format, size, InitializeBackingStore::No));
    raw_data.copy_to(Bytes { backing_store.data, backing_store.size_in_bytes });
    return AK::adopt_nonnull_ref_or_enomem(new (nothrow) Bitmap(format, alpha_type, size, backing_store));
}

Bitmap::Bitmap(BitmapFormat format, AlphaType alpha_type, Core::AnonymousBuffer buffer, IntSize size)
    : m_size(size)
    , m_data(buffer.data<void>())
    , m_pitch(minimum_pitch(size.width(), format))
    , m_format(format)
    , m_alpha_type(alpha_type)
    , m_buffer(move(buffer))
{
    VERIFY(!size_would_overflow(format, size));
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::clone() const
{
    auto new_bitmap = TRY(Bitmap::create(format(), alpha_type(), size()));

    for (int y = 0; y < height(); ++y)
        memcpy(new_bitmap->scanline_u8(y), scanline_u8(y), new_bitmap->pitch());

    return new_bitmap;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::cropped(Gfx::IntRect crop, Gfx::Color outside_color) const
{
    // OPTIMIZATION: Skip slow manual copying for NO-OP crops
    if (crop == rect())
        return clone();

    auto new_bitmap = TRY(Gfx::Bitmap::create(format(), alpha_type(), { crop.width(), crop.height() }));

    for (int y = 0; y < crop.height(); ++y) {
        for (int x = 0; x < crop.width(); ++x) {
            int global_x = x + crop.left();
            int global_y = y + crop.top();
            if (global_x >= width() || global_y >= height() || global_x < 0 || global_y < 0) {
                new_bitmap->set_pixel(x, y, outside_color);
            } else {
                new_bitmap->set_pixel(x, y, get_pixel(global_x, global_y));
            }
        }
    }
    return new_bitmap;
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::to_bitmap_backed_by_anonymous_buffer() const
{
    if (m_buffer.is_valid()) {
        // FIXME: The const_cast here is awkward.
        return NonnullRefPtr { const_cast<Bitmap&>(*this) };
    }
    auto bitmap = TRY(Bitmap::create_shareable(format(), alpha_type(), size()));
    for (int y = 0; y < height(); ++y)
        memcpy(bitmap->scanline(y), scanline(y), bitmap->pitch());
    return bitmap;
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::to_bitmap_shareable_with_gpu_access() const
{
    if (is_in_shareable_memory()) {
        // FIXME: The const_cast here is awkward.
        return NonnullRefPtr { const_cast<Bitmap&>(*this) };
    }
    auto bitmap = TRY(Bitmap::create_shareable_with_gpu_access(format(), alpha_type(), size()));
    auto const row_size = minimum_pitch(width(), format());
    for (int y = 0; y < height(); ++y)
        memcpy(bitmap->scanline_u8(y), scanline_u8(y), row_size);
    return bitmap;
}

Bitmap::~Bitmap()
{
    if (m_destruction_callback)
        m_destruction_callback();
    m_data = nullptr;
}

Gfx::ShareableBitmap Bitmap::to_shareable_bitmap() const
{
    auto bitmap_or_error = to_bitmap_backed_by_anonymous_buffer();
    if (bitmap_or_error.is_error())
        return {};
    return Gfx::ShareableBitmap { bitmap_or_error.release_value_but_fixme_should_propagate_errors(), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

ErrorOr<BackingStore> Bitmap::allocate_backing_store(BitmapFormat format, IntSize size, InitializeBackingStore initialize_backing_store)
{
    if (size.is_empty())
        return Error::from_string_literal("Gfx::Bitmap backing store size is empty");

    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap backing store size overflow");

    auto const pitch = minimum_pitch(size.width(), format);
    auto const data_size_in_bytes = size_in_bytes(pitch, size.height());

    void* data;
    if (initialize_backing_store == InitializeBackingStore::Yes)
        data = kcalloc(1, data_size_in_bytes);
    else
        data = kmalloc(data_size_in_bytes);
    if (data == nullptr)
        return Error::from_errno(errno);
    return BackingStore { data, pitch, data_size_in_bytes };
}

Bitmap::DiffResult Bitmap::diff(Bitmap const& other) const
{
    auto own_width = width();
    auto own_height = height();
    VERIFY(own_width == other.width() && own_height == other.height());

    DiffResult result;
    for (auto y = 0; y < own_height; ++y) {
        for (auto x = 0; x < own_width; ++x) {
            auto own_pixel = get_pixel(x, y);
            auto other_pixel = other.get_pixel(x, y);
            if (own_pixel == other_pixel)
                continue;

            ++result.pixel_error_count;

            u8 red_error = abs(static_cast<int>(own_pixel.red()) - other_pixel.red());
            u8 green_error = abs(static_cast<int>(own_pixel.green()) - other_pixel.green());
            u8 blue_error = abs(static_cast<int>(own_pixel.blue()) - other_pixel.blue());
            u8 alpha_error = abs(static_cast<int>(own_pixel.alpha()) - other_pixel.alpha());

            result.total_red_error += red_error;
            result.total_green_error += green_error;
            result.total_blue_error += blue_error;
            result.total_alpha_error += alpha_error;

            result.maximum_red_error = max(result.maximum_red_error, red_error);
            result.maximum_green_error = max(result.maximum_green_error, green_error);
            result.maximum_blue_error = max(result.maximum_blue_error, blue_error);
            result.maximum_alpha_error = max(result.maximum_alpha_error, alpha_error);
        }
    }

    result.identical = result.pixel_error_count == 0;
    result.total_error = result.total_red_error + result.total_green_error + result.total_blue_error + result.total_alpha_error;

    u8 maximum_red_green_error = max(result.maximum_red_error, result.maximum_green_error);
    u8 maximum_blue_alpha_error = max(result.maximum_blue_error, result.maximum_alpha_error);
    result.maximum_error = max(maximum_red_green_error, maximum_blue_alpha_error);

    return result;
}

void Bitmap::set_alpha_type_destructive(AlphaType alpha_type)
{
    if (alpha_type == m_alpha_type)
        return;

    if (m_format == BitmapFormat::BGRx8888 || m_format == BitmapFormat::RGBx8888) {
        m_alpha_type = alpha_type;
        return;
    }

#ifdef AK_OS_MACOS
    vImage_Buffer buf { .data = m_data, .height = vImagePixelCount(height()), .width = vImagePixelCount(width()), .rowBytes = pitch() };
    vImage_Error err;
    if (m_alpha_type == AlphaType::Unpremultiplied) {
        switch (m_format) {
        case BitmapFormat::BGRA8888:
            err = vImagePremultiplyData_BGRA8888(&buf, &buf, kvImageNoFlags);
            break;
        case BitmapFormat::RGBA8888:
            err = vImagePremultiplyData_RGBA8888(&buf, &buf, kvImageNoFlags);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    } else {
        switch (m_format) {
        case BitmapFormat::BGRA8888:
            err = vImageUnpremultiplyData_BGRA8888(&buf, &buf, kvImageNoFlags);
            break;
        case BitmapFormat::RGBA8888:
            err = vImageUnpremultiplyData_RGBA8888(&buf, &buf, kvImageNoFlags);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    VERIFY(err == kvImageNoError);
#else
    VERIFY(m_format == BitmapFormat::BGRA8888 || m_format == BitmapFormat::RGBA8888);
    // Both formats keep alpha in the last byte, and their color channels convert the same way. The arithmetic is the
    // same as Skia's, so a bitmap converted here matches one that Skia converts.
    for (int y = 0; y < height(); ++y) {
        auto* pixel = scanline_u8(y);
        for (int x = 0; x < width(); ++x, pixel += 4) {
            auto alpha = pixel[3];
            if (alpha == 255)
                continue;
            if (alpha_type == AlphaType::Premultiplied) {
                for (int channel = 0; channel < 3; ++channel)
                    pixel[channel] = (pixel[channel] * alpha + 127) / 255;
                continue;
            }
            float scale = alpha == 0 ? 0.0f : 1.0f / (alpha * (1 / 255.0f));
            for (int channel = 0; channel < 3; ++channel) {
                float value = pixel[channel] * (1 / 255.0f) * scale;
                pixel[channel] = static_cast<u8>(__builtin_rintf(clamp(value * 255.0f, 0.0f, 255.0f)));
            }
        }
    }
#endif
    m_alpha_type = alpha_type;
}

}
