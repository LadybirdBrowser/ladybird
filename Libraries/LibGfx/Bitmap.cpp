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
#include <LibGfx/SkiaUtils.h>

#include <core/SkBitmap.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkImageInfo.h>
#include <core/SkPixmap.h>
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

static bool size_would_overflow(BitmapFormat format, IntSize size)
{
    if (size.width() < 0 || size.height() < 0)
        return true;
    // This check is a bit arbitrary, but should protect us from most shenanigans:
    if (size.width() >= INT16_MAX || size.height() >= INT16_MAX)
        return true;
    // In contrast, this check is absolutely necessary:
    size_t pitch = Bitmap::minimum_pitch(size.width(), format);
    return Checked<size_t>::multiplication_would_overflow(pitch, size.height());
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
    m_destruction_callback = [data = m_data, size_in_bytes = this->size_in_bytes()] {
        kfree_sized(data, size_in_bytes);
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

    return adopt_nonnull_ref_or_enomem(new (nothrow) Bitmap(format, alpha_type, move(buffer), size));
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_with_raw_data(BitmapFormat format, AlphaType alpha_type, ReadonlyBytes raw_data, IntSize size)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_with_raw_data size overflow");

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

    VERIFY(size_in_bytes() == new_bitmap->size_in_bytes());
    memcpy(new_bitmap->scanline(0), scanline(0), size_in_bytes());

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

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::scaled(int const width, int const height, ScalingMode const scaling_mode) const
{
    auto const source_info = SkImageInfo::Make(this->width(), this->height(), to_skia_color_type(format()), to_skia_alpha_type(format(), alpha_type()), nullptr);
    SkPixmap const source_sk_pixmap(source_info, begin(), pitch());
    SkBitmap source_sk_bitmap;
    source_sk_bitmap.installPixels(source_sk_pixmap);
    source_sk_bitmap.setImmutable();

    auto scaled_bitmap = TRY(Gfx::Bitmap::create(format(), alpha_type(), { width, height }));
    auto const scaled_info = SkImageInfo::Make(scaled_bitmap->width(), scaled_bitmap->height(), to_skia_color_type(scaled_bitmap->format()), to_skia_alpha_type(scaled_bitmap->format(), scaled_bitmap->alpha_type()), nullptr);
    SkPixmap const scaled_sk_pixmap(scaled_info, scaled_bitmap->begin(), scaled_bitmap->pitch());

    sk_sp<SkImage> source_sk_image = source_sk_bitmap.asImage();
    if (!source_sk_image->scalePixels(scaled_sk_pixmap, to_skia_sampling_options(scaling_mode)))
        return Error::from_string_literal("Unable to scale pixels for bitmap");
    return scaled_bitmap;
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::to_bitmap_backed_by_anonymous_buffer() const
{
    if (m_buffer.is_valid()) {
        // FIXME: The const_cast here is awkward.
        return NonnullRefPtr { const_cast<Bitmap&>(*this) };
    }
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(round_up_to_power_of_two(size_in_bytes(), PAGE_SIZE)));
    auto bitmap = TRY(Bitmap::create_with_anonymous_buffer(format(), alpha_type(), move(buffer), size()));
    memcpy(bitmap->scanline(0), scanline(0), size_in_bytes());
    return bitmap;
}

Bitmap::~Bitmap()
{
    if (m_destruction_callback)
        m_destruction_callback();
    m_data = nullptr;
}

void Bitmap::strip_alpha_channel()
{
    VERIFY(m_format == BitmapFormat::BGRA8888 || m_format == BitmapFormat::BGRx8888);
    for (BGRA8888& pixel : *this)
        pixel = 0xff000000 | (pixel & 0xffffff);
    m_format = BitmapFormat::BGRx8888;
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
    auto color_type = to_skia_color_type(m_format);
    auto source_alpha = to_skia_alpha_type(m_format, m_alpha_type);
    auto destination_alpha = to_skia_alpha_type(m_format, alpha_type);

    auto color_space = SkColorSpace::MakeSRGB();

    auto source_info = SkImageInfo::Make(width(), height(), color_type, source_alpha, color_space);
    auto destination_info = SkImageInfo::Make(width(), height(), color_type, destination_alpha, color_space);

    SkPixmap src_pixmap(source_info, m_data, pitch());
    SkPixmap dst_pixmap(destination_info, m_data, pitch());

    bool ok = src_pixmap.readPixels(dst_pixmap);
    VERIFY(ok);
#endif
    m_alpha_type = alpha_type;
}

}
