/*
 * Copyright (c) 2018-2024, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Timothy Slater <tslater2006@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/LexicalPath.h>
#include <AK/Memory.h>
#include <AK/MemoryStream.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/MimeData.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
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
    auto backing_store = TRY(Bitmap::allocate_backing_store(format, size));
    return AK::adopt_nonnull_ref_or_enomem(new (nothrow) Bitmap(format, size, backing_store));
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_shareable(BitmapFormat format, IntSize size)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_shareable size overflow");

    auto const pitch = minimum_pitch(size.width(), format);
    auto const data_size = size_in_bytes(pitch, size.height());

    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(round_up_to_power_of_two(data_size, PAGE_SIZE)));
    auto bitmap = TRY(Bitmap::create_with_anonymous_buffer(format, buffer, size));
    return bitmap;
}

Bitmap::Bitmap(BitmapFormat format, IntSize size, BackingStore const& backing_store)
    : m_size(size)
    , m_data(backing_store.data)
    , m_pitch(backing_store.pitch)
    , m_format(format)
{
    VERIFY(!m_size.is_empty());
    VERIFY(!size_would_overflow(format, size));
    VERIFY(m_data);
    VERIFY(backing_store.size_in_bytes == size_in_bytes());
    m_destruction_callback = [data = m_data, size_in_bytes = this->size_in_bytes()] {
        kfree_sized(data, size_in_bytes);
    };
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_wrapper(BitmapFormat format, IntSize size, size_t pitch, void* data, Function<void()>&& destruction_callback)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_wrapper size overflow");
    return adopt_ref(*new Bitmap(format, size, pitch, data, move(destruction_callback)));
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::load_from_file(StringView path, Optional<IntSize> ideal_size)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    return load_from_file(move(file), path, ideal_size);
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::load_from_file(NonnullOwnPtr<Core::File> file, StringView path, Optional<IntSize> ideal_size)
{
    auto mapped_file = TRY(Core::MappedFile::map_from_file(move(file), path));
    auto mime_type = Core::guess_mime_type_based_on_filename(path);
    return load_from_bytes(mapped_file->bytes(), ideal_size, mime_type);
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::load_from_bytes(ReadonlyBytes bytes, Optional<IntSize> ideal_size, Optional<ByteString> mine_type)
{
    if (auto decoder = TRY(ImageDecoder::try_create_for_raw_bytes(bytes, mine_type))) {
        auto frame = TRY(decoder->frame(0, ideal_size));
        if (auto& bitmap = frame.image)
            return bitmap.release_nonnull();
    }

    return Error::from_string_literal("Gfx::Bitmap unable to load from file");
}

Bitmap::Bitmap(BitmapFormat format, IntSize size, size_t pitch, void* data, Function<void()>&& destruction_callback)
    : m_size(size)
    , m_data(data)
    , m_pitch(pitch)
    , m_format(format)
    , m_destruction_callback(move(destruction_callback))
{
    VERIFY(pitch >= minimum_pitch(size.width(), format));
    VERIFY(!size_would_overflow(format, size));
    // FIXME: assert that `data` is actually long enough!
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_with_anonymous_buffer(BitmapFormat format, Core::AnonymousBuffer buffer, IntSize size)
{
    if (size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::Bitmap::create_with_anonymous_buffer size overflow");

    return adopt_nonnull_ref_or_enomem(new (nothrow) Bitmap(format, move(buffer), size));
}

Bitmap::Bitmap(BitmapFormat format, Core::AnonymousBuffer buffer, IntSize size)
    : m_size(size)
    , m_data(buffer.data<void>())
    , m_pitch(minimum_pitch(size.width(), format))
    , m_format(format)
    , m_buffer(move(buffer))
{
    VERIFY(!size_would_overflow(format, size));
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::clone() const
{
    auto new_bitmap = TRY(Bitmap::create(format(), size()));

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

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::scaled(int sx, int sy) const
{
    VERIFY(sx >= 0 && sy >= 0);
    if (sx == 1 && sy == 1)
        return clone();

    auto new_bitmap = TRY(Gfx::Bitmap::create(format(), { width() * sx, height() * sy }));

    auto old_width = width();
    auto old_height = height();

    for (int y = 0; y < old_height; y++) {
        for (int x = 0; x < old_width; x++) {
            auto color = get_pixel(x, y);

            auto base_x = x * sx;
            auto base_y = y * sy;
            for (int new_y = base_y; new_y < base_y + sy; new_y++) {
                for (int new_x = base_x; new_x < base_x + sx; new_x++) {
                    new_bitmap->set_pixel(new_x, new_y, color);
                }
            }
        }
    }

    return new_bitmap;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::scaled(float sx, float sy) const
{
    VERIFY(sx >= 0.0f && sy >= 0.0f);
    if (floorf(sx) == sx && floorf(sy) == sy)
        return scaled(static_cast<int>(sx), static_cast<int>(sy));

    int scaled_width = (int)ceilf(sx * (float)width());
    int scaled_height = (int)ceilf(sy * (float)height());
    return scaled_to_size({ scaled_width, scaled_height });
}

// http://fourier.eng.hmc.edu/e161/lectures/resize/node3.html
ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::scaled_to_size(Gfx::IntSize size) const
{
    auto new_bitmap = TRY(Gfx::Bitmap::create(format(), size));

    auto old_width = width();
    auto old_height = height();
    auto new_width = new_bitmap->width();
    auto new_height = new_bitmap->height();

    if (old_width == 1 && old_height == 1) {
        new_bitmap->fill(get_pixel(0, 0));
        return new_bitmap;
    }

    if (old_width > 1 && old_height > 1) {
        // The interpolation goes out of bounds on the bottom- and right-most edges.
        // We handle those in two specialized loops not only to make them faster, but
        // also to avoid four branch checks for every pixel.
        for (int y = 0; y < new_height - 1; y++) {
            for (int x = 0; x < new_width - 1; x++) {
                auto p = static_cast<float>(x) * static_cast<float>(old_width - 1) / static_cast<float>(new_width - 1);
                auto q = static_cast<float>(y) * static_cast<float>(old_height - 1) / static_cast<float>(new_height - 1);

                int i = floorf(p);
                int j = floorf(q);
                float u = p - static_cast<float>(i);
                float v = q - static_cast<float>(j);

                auto a = get_pixel(i, j);
                auto b = get_pixel(i + 1, j);
                auto c = get_pixel(i, j + 1);
                auto d = get_pixel(i + 1, j + 1);

                auto e = a.mixed_with(b, u);
                auto f = c.mixed_with(d, u);
                auto color = e.mixed_with(f, v);
                new_bitmap->set_pixel(x, y, color);
            }
        }

        // Bottom strip (excluding last pixel)
        auto old_bottom_y = old_height - 1;
        auto new_bottom_y = new_height - 1;
        for (int x = 0; x < new_width - 1; x++) {
            auto p = static_cast<float>(x) * static_cast<float>(old_width - 1) / static_cast<float>(new_width - 1);

            int i = floorf(p);
            float u = p - static_cast<float>(i);

            auto a = get_pixel(i, old_bottom_y);
            auto b = get_pixel(i + 1, old_bottom_y);
            auto color = a.mixed_with(b, u);
            new_bitmap->set_pixel(x, new_bottom_y, color);
        }

        // Right strip (excluding last pixel)
        auto old_right_x = old_width - 1;
        auto new_right_x = new_width - 1;
        for (int y = 0; y < new_height - 1; y++) {
            auto q = static_cast<float>(y) * static_cast<float>(old_height - 1) / static_cast<float>(new_height - 1);

            int j = floorf(q);
            float v = q - static_cast<float>(j);

            auto c = get_pixel(old_right_x, j);
            auto d = get_pixel(old_right_x, j + 1);

            auto color = c.mixed_with(d, v);
            new_bitmap->set_pixel(new_right_x, y, color);
        }

        // Bottom-right pixel
        new_bitmap->set_pixel(new_width - 1, new_height - 1, get_pixel(width() - 1, height() - 1));
        return new_bitmap;
    } else if (old_height == 1) {
        // Copy horizontal strip multiple times (excluding last pixel to out of bounds).
        auto old_bottom_y = old_height - 1;
        for (int x = 0; x < new_width - 1; x++) {
            auto p = static_cast<float>(x) * static_cast<float>(old_width - 1) / static_cast<float>(new_width - 1);
            int i = floorf(p);
            float u = p - static_cast<float>(i);

            auto a = get_pixel(i, old_bottom_y);
            auto b = get_pixel(i + 1, old_bottom_y);
            auto color = a.mixed_with(b, u);
            for (int new_bottom_y = 0; new_bottom_y < new_height; new_bottom_y++) {
                // Interpolate color only once and then copy into all columns.
                new_bitmap->set_pixel(x, new_bottom_y, color);
            }
        }
        for (int new_bottom_y = 0; new_bottom_y < new_height; new_bottom_y++) {
            // Copy last pixel of horizontal strip
            new_bitmap->set_pixel(new_width - 1, new_bottom_y, get_pixel(width() - 1, old_bottom_y));
        }
        return new_bitmap;
    } else if (old_width == 1) {
        // Copy vertical strip multiple times (excluding last pixel to avoid out of bounds).
        auto old_right_x = old_width - 1;
        for (int y = 0; y < new_height - 1; y++) {
            auto q = static_cast<float>(y) * static_cast<float>(old_height - 1) / static_cast<float>(new_height - 1);
            int j = floorf(q);
            float v = q - static_cast<float>(j);

            auto c = get_pixel(old_right_x, j);
            auto d = get_pixel(old_right_x, j + 1);

            auto color = c.mixed_with(d, v);
            for (int new_right_x = 0; new_right_x < new_width; new_right_x++) {
                // Interpolate color only once and copy into all rows.
                new_bitmap->set_pixel(new_right_x, y, color);
            }
        }
        for (int new_right_x = 0; new_right_x < new_width; new_right_x++) {
            // Copy last pixel of vertical strip
            new_bitmap->set_pixel(new_right_x, new_height - 1, get_pixel(old_right_x, height() - 1));
        }
    }
    return new_bitmap;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> Bitmap::cropped(Gfx::IntRect crop, Optional<BitmapFormat> new_bitmap_format) const
{
    auto new_bitmap = TRY(Gfx::Bitmap::create(new_bitmap_format.value_or(format()), { crop.width(), crop.height() }));

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
    auto bitmap = TRY(Bitmap::create_with_anonymous_buffer(m_format, move(buffer), size()));
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

void Bitmap::fill(Color color)
{
    for (int y = 0; y < height(); ++y) {
        auto* scanline = this->scanline(y);
        fast_u32_fill(scanline, color.value(), width());
    }
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
