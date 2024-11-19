/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
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

namespace Gfx {

struct BackingStore {
    void* data { nullptr };
    size_t pitch { 0 };
    size_t size_in_bytes { 0 };
};

size_t Bitmap::minimum_pitch(size_t width, BitmapFormat format)
{
    size_t element_size;
    switch (determine_storage_format(format)) {
    case StorageFormat::BGRx8888:
    case StorageFormat::BGRA8888:
    case StorageFormat::RGBx8888:
    case StorageFormat::RGBA8888:
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

void Bitmap::apply_mask(Gfx::Bitmap const& mask, MaskKind mask_kind)
{
    VERIFY(size() == mask.size());

    for (int y = 0; y < height(); y++) {
        for (int x = 0; x < width(); x++) {
            auto color = get_pixel(x, y);
            auto mask_color = mask.get_pixel(x, y);
            if (mask_kind == MaskKind::Luminance) {
                color = color.with_alpha(color.alpha() * mask_color.alpha() * mask_color.luminosity() / (255 * 255));
            } else {
                VERIFY(mask_kind == MaskKind::Alpha);
                color = color.with_alpha(color.alpha() * mask_color.alpha() / 255);
            }
            set_pixel(x, y, color);
        }
    }
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::cropped(Gfx::IntRect crop, Optional<BitmapFormat> new_bitmap_format) const
{
    auto new_bitmap = TRY(Gfx::Bitmap::create(new_bitmap_format.value_or(format()), alpha_type(), { crop.width(), crop.height() }));

    for (int y = 0; y < crop.height(); ++y) {
        for (int x = 0; x < crop.width(); ++x) {
            int global_x = x + crop.left();
            int global_y = y + crop.top();
            if (global_x >= width() || global_y >= height() || global_x < 0 || global_y < 0) {
                new_bitmap->set_pixel(x, y, Gfx::Color::Black);
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
    for (ARGB32& pixel : *this)
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

ErrorOr<BackingStore> Bitmap::allocate_backing_store(BitmapFormat format, IntSize size)
{
    if (size.is_empty())
        return Error::from_string_literal("Gfx::Bitmap backing store size is empty");

    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap backing store size overflow");

    auto const pitch = minimum_pitch(size.width(), format);
    auto const data_size_in_bytes = size_in_bytes(pitch, size.height());

    void* data = kcalloc(1, data_size_in_bytes);
    if (data == nullptr)
        return Error::from_errno(errno);
    return BackingStore { data, pitch, data_size_in_bytes };
}

bool Bitmap::visually_equals(Bitmap const& other) const
{
    auto own_width = width();
    auto own_height = height();
    if (other.width() != own_width || other.height() != own_height)
        return false;

    for (auto y = 0; y < own_height; ++y) {
        for (auto x = 0; x < own_width; ++x) {
            if (get_pixel(x, y) != other.get_pixel(x, y))
                return false;
        }
    }

    return true;
}

}
