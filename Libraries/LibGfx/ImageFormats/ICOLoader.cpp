/*
 * Copyright (c) 2020, Paul Roukema <roukemap@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/MemoryStream.h>
#include <AK/Types.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/PNGLoader.h>

namespace Gfx {

enum class IconType : u16 {
    ICO = 1,
    CUR = 2,
};

// FIXME: This is in little-endian order. Maybe need a NetworkOrdered<T> equivalent eventually.
struct ICONDIR {
    u16 must_be_0 = 0;
    IconType type = IconType::ICO;
    u16 image_count = 0;
};
static_assert(AssertSize<ICONDIR, 6>());

struct ICONDIRENTRY {
    u8 width;
    u8 height;
    u8 color_count;
    u8 reserved_0;
    u16 planes;
    u16 bits_per_pixel;
    u32 size;
    u32 offset;
};
static_assert(AssertSize<ICONDIRENTRY, 16>());

};

template<>
class AK::Traits<Gfx::ICONDIR> : public DefaultTraits<Gfx::ICONDIR> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
class AK::Traits<Gfx::ICONDIRENTRY> : public DefaultTraits<Gfx::ICONDIRENTRY> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

namespace Gfx {

struct ICOImageDescriptor {
    u16 width;
    u16 height;
    u16 bits_per_pixel;
    u16 hotspot_x { 0 };
    u16 hotspot_y { 0 };
    size_t offset;
    size_t size;
    RefPtr<Gfx::Bitmap> bitmap;
};

struct ICOLoadingContext {
    ICOLoadingContext(NonnullRefPtr<ImageDecoderStream> stream)
        : stream(move(stream))
    {
    }

    enum State {
        NotDecoded = 0,
        Error,
        DirectoryDecoded,
        BitmapDecoded
    };
    State state { NotDecoded };
    NonnullRefPtr<ImageDecoderStream> stream;
    IconType file_type { IconType::ICO };
    Vector<ICOImageDescriptor> images;
    size_t largest_index;
};

static ErrorOr<size_t> decode_ico_header(Stream& stream, IconType& out_type)
{
    auto header = TRY(stream.read_value<ICONDIR>());
    if (header.must_be_0 != 0 || (header.type != IconType::ICO && header.type != IconType::CUR))
        return Error::from_string_literal("Invalid ICO/CUR header");

    out_type = header.type;
    return { header.image_count };
}

static ErrorOr<ICOImageDescriptor> decode_ico_direntry(Stream& stream, IconType file_type)
{
    auto entry = TRY(stream.read_value<ICONDIRENTRY>());
    ICOImageDescriptor desc = {
        entry.width,
        entry.height,
        entry.bits_per_pixel,
        0, 0,
        entry.offset,
        entry.size,
        nullptr
    };

    if (file_type == IconType::CUR) {
        // For cursor files, hotsport coordinates are stored in the planes and bits_per_pixel fields
        desc.hotspot_x = entry.planes;
        desc.hotspot_y = entry.bits_per_pixel;
    }

    if (desc.width == 0)
        desc.width = 256;
    if (desc.height == 0)
        desc.height = 256;

    return { desc };
}

static size_t find_largest_image(ICOLoadingContext const& context)
{
    size_t max_area = 0;
    size_t index = 0;
    size_t largest_index = 0;
    u16 max_bits_per_pixel = 0;
    for (auto const& desc : context.images) {
        if (static_cast<size_t>(desc.width) * static_cast<size_t>(desc.height) >= max_area) {
            if (desc.bits_per_pixel > max_bits_per_pixel) {
                max_area = desc.width * desc.height;
                largest_index = index;
                max_bits_per_pixel = desc.bits_per_pixel;
            }
        }
        ++index;
    }
    return largest_index;
}

static ErrorOr<void> load_ico_directory(ICOLoadingContext& context)
{
    auto image_count = TRY(decode_ico_header(context.stream, context.file_type));
    if (image_count == 0)
        return Error::from_string_literal("ICO/CUR file has no images");

    for (size_t i = 0; i < image_count; ++i) {
        auto desc = TRY(decode_ico_direntry(context.stream, context.file_type));
        if (Checked<size_t>::addition_would_overflow(desc.offset, desc.size)) {
            dbgln_if(ICO_DEBUG, "load_ico_directory: offset: {} size: {} doesn't fit in ICO size", desc.offset, desc.size);
            return Error::from_string_literal("ICO size too large");
        }
        dbgln_if(ICO_DEBUG, "load_ico_directory: index {} width: {} height: {} offset: {} size: {}", i, desc.width, desc.height, desc.offset, desc.size);
        TRY(context.images.try_append(desc));
    }
    context.largest_index = find_largest_image(context);
    context.state = ICOLoadingContext::State::DirectoryDecoded;
    return {};
}

ErrorOr<void> ICOImageDecoderPlugin::load_ico_bitmap(ICOLoadingContext& context)
{
    VERIFY(context.state >= ICOLoadingContext::State::DirectoryDecoded);

    size_t const real_index = context.largest_index;
    if (real_index >= context.images.size())
        return Error::from_string_literal("Index out of bounds");

    ICOImageDescriptor& desc = context.images[real_index];
    TRY(context.stream->seek(desc.offset, SeekMode::SetPosition));

    auto desc_stream = TRY(adopt_nonnull_ref_or_enomem(new ImageDecoderStream()));
    auto desc_bytes = TRY(ByteBuffer::create_uninitialized(desc.size));
    TRY(context.stream->read_until_filled(desc_bytes));
    desc_stream->append_chunk(move(desc_bytes));
    desc_stream->close();

    if (PNGImageDecoderPlugin::sniff(desc_stream)) {
        TRY(desc_stream->seek(0, SeekMode::SetPosition));
        auto png_decoder = TRY(PNGImageDecoderPlugin::create(desc_stream));
        auto decoded_png_frame = TRY(png_decoder->frame(0));
        desc.bitmap = decoded_png_frame.image;
        return {};
    }

    TRY(desc_stream->seek(0, SeekMode::SetPosition));
    auto bmp_decoder = TRY(BMPImageDecoderPlugin::create_as_included_in_ico({}, desc_stream));
    // NOTE: We don't initialize a BMP decoder in the usual way, but rather
    // we just create an object and try to sniff for a frame when it's included
    // inside an ICO image.
    if (bmp_decoder->sniff_dib()) {
        auto decoded_bmp_frame = TRY(bmp_decoder->frame(0));
        desc.bitmap = decoded_bmp_frame.image;
    } else {
        dbgln_if(ICO_DEBUG, "load_ico_bitmap: encoded image not supported at index: {}", real_index);
        return Error::from_string_literal("Encoded image not supported");
    }
    return {};
}

bool ICOImageDecoderPlugin::sniff(NonnullRefPtr<ImageDecoderStream> stream)
{
    IconType file_type;
    return !decode_ico_header(stream, file_type).is_error();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> ICOImageDecoderPlugin::create(NonnullRefPtr<ImageDecoderStream> stream)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) ICOImageDecoderPlugin(move(stream))));
    TRY(load_ico_directory(*plugin->m_context));
    return plugin;
}

ICOImageDecoderPlugin::ICOImageDecoderPlugin(NonnullRefPtr<ImageDecoderStream> stream)
{
    m_context = make<ICOLoadingContext>(move(stream));
}

ICOImageDecoderPlugin::~ICOImageDecoderPlugin() = default;

IntSize ICOImageDecoderPlugin::size()
{
    return { m_context->images[m_context->largest_index].width, m_context->images[m_context->largest_index].height };
}

ErrorOr<ImageFrameDescriptor> ICOImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("ICOImageDecoderPlugin: Invalid frame index");

    if (m_context->state == ICOLoadingContext::State::Error)
        return Error::from_string_literal("ICOImageDecoderPlugin: Decoding failed");

    if (m_context->state < ICOLoadingContext::State::BitmapDecoded) {
        // NOTE: This forces the chunk decoding to happen.
        auto maybe_error = load_ico_bitmap(*m_context);
        if (maybe_error.is_error()) {
            m_context->state = ICOLoadingContext::State::Error;
            return Error::from_string_literal("ICOImageDecoderPlugin: Decoding failed");
        }
        m_context->state = ICOLoadingContext::State::BitmapDecoded;
    }

    VERIFY(m_context->images[m_context->largest_index].bitmap);
    return ImageFrameDescriptor { *m_context->images[m_context->largest_index].bitmap, 0 };
}

}
