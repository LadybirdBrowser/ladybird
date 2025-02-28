/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BitStream.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/Error.h>
#include <AK/IntegralMath.h>
#include <AK/Memory.h>
#include <AK/MemoryStream.h>
#include <AK/Try.h>
#include <LibCompress/Lzw.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/Painter.h>

namespace Gfx {

// Row strides and offsets for each interlace pass.
static constexpr Array<int, 4> INTERLACE_ROW_STRIDES = { 8, 8, 4, 2 };
static constexpr Array<int, 4> INTERLACE_ROW_OFFSETS = { 0, 4, 2, 1 };

struct GIFImageDescriptor {
    u16 x { 0 };
    u16 y { 0 };
    u16 width { 0 };
    u16 height { 0 };
    bool use_global_color_map { true };
    bool interlaced { false };
    Color color_map[256];
    u8 lzw_min_code_size { 0 };
    ByteBuffer lzw_encoded_bytes;

    // Fields from optional graphic control extension block
    enum DisposalMethod : u8 {
        None = 0,
        InPlace = 1,
        RestoreBackground = 2,
        RestorePrevious = 3,
    };
    DisposalMethod disposal_method { None };
    u8 transparency_index { 0 };
    u16 duration { 0 };
    bool transparent { false };
    bool user_input { false };

    IntRect rect() const
    {
        return { this->x, this->y, this->width, this->height };
    }
};

struct LogicalScreen {
    u16 width;
    u16 height;
    Color color_map[256];
};

struct GIFLoadingContext {
    GIFLoadingContext(FixedMemoryStream stream)
        : stream(move(stream))
    {
    }

    enum State {
        NotDecoded = 0,
        FrameDescriptorsLoaded,
        FrameComplete,
    };
    State state { NotDecoded };
    enum ErrorState {
        NoError = 0,
        FailedToDecodeAllFrames,
        FailedToDecodeAnyFrame,
        FailedToLoadFrameDescriptors,
    };
    ErrorState error_state { NoError };

    FixedMemoryStream stream;

    LogicalScreen logical_screen {};
    u8 background_color_index { 0 };
    Vector<NonnullOwnPtr<GIFImageDescriptor>> images {};
    size_t loops { 1 };
    RefPtr<Gfx::Bitmap> frame_buffer;
    size_t current_frame { 0 };
    RefPtr<Gfx::Bitmap> prev_frame_buffer;
};

enum class GIFFormat {
    GIF87a,
    GIF89a,
};

static ErrorOr<GIFFormat> decode_gif_header(Stream& stream)
{
    static auto valid_header_87 = "GIF87a"sv;
    static auto valid_header_89 = "GIF89a"sv;

    Array<u8, 6> header;
    TRY(stream.read_until_filled(header));

    if (header.span() == valid_header_87.bytes())
        return GIFFormat::GIF87a;
    if (header.span() == valid_header_89.bytes())
        return GIFFormat::GIF89a;

    return Error::from_string_literal("GIF header unknown");
}

static void copy_frame_buffer(Bitmap& dest, Bitmap const& src)
{
    VERIFY(dest.size_in_bytes() == src.size_in_bytes());
    memcpy(dest.scanline(0), src.scanline(0), dest.size_in_bytes());
}

static void clear_rect(Bitmap& bitmap, IntRect const& rect, Color color)
{
    auto intersection_rect = rect.intersected(bitmap.rect());
    if (intersection_rect.is_empty())
        return;

    ARGB32* dst = bitmap.scanline(intersection_rect.top()) + intersection_rect.left();
    size_t const dst_skip = bitmap.pitch() / sizeof(ARGB32);

    auto const value = color.value();
    auto const width = intersection_rect.width();
    for (int i = intersection_rect.height() - 1; i >= 0; --i) {
        for (ARGB32* p = dst; p < (dst + width); ++p) {
            *p = value;
        }
        dst += dst_skip;
    }
}

static ErrorOr<void> decode_frame(GIFLoadingContext& context, size_t frame_index)
{
    if (frame_index >= context.images.size()) {
        return Error::from_string_literal("frame_index size too high");
    }

    if (context.state >= GIFLoadingContext::State::FrameComplete && frame_index == context.current_frame) {
        return {};
    }

    size_t start_frame = context.current_frame + 1;
    if (context.state < GIFLoadingContext::State::FrameComplete) {
        start_frame = 0;
        context.frame_buffer = TRY(Bitmap::create(BitmapFormat::BGRA8888, { context.logical_screen.width, context.logical_screen.height }));
        context.prev_frame_buffer = TRY(Bitmap::create(BitmapFormat::BGRA8888, { context.logical_screen.width, context.logical_screen.height }));

    } else if (frame_index < context.current_frame) {
        start_frame = 0;
    }

    for (size_t i = start_frame; i <= frame_index; ++i) {
        auto& image = context.images.at(i);

        auto const previous_image_disposal_method = i > 0 ? context.images.at(i - 1)->disposal_method : GIFImageDescriptor::DisposalMethod::None;

        if (i == 0) {
            auto painter = Gfx::Painter::create(*context.frame_buffer);
            painter->clear_rect(context.frame_buffer->rect().to_type<float>(), Color::Transparent);
        } else if (i > 0 && image->disposal_method == GIFImageDescriptor::DisposalMethod::RestorePrevious
            && previous_image_disposal_method != GIFImageDescriptor::DisposalMethod::RestorePrevious) {
            // This marks the start of a run of frames that once disposed should be restored to the
            // previous underlying image contents. Therefore we make a copy of the current frame
            // buffer so that it can be restored later.
            copy_frame_buffer(*context.prev_frame_buffer, *context.frame_buffer);
        }

        if (previous_image_disposal_method == GIFImageDescriptor::DisposalMethod::RestoreBackground) {
            // Note: RestoreBackground could be interpreted either as restoring the underlying
            // background of the entire image (e.g. container element's background-color), or the
            // background color of the GIF itself. It appears that all major browsers and most other
            // GIF decoders adhere to the former interpretation, therefore we will do the same by
            // clearing the entire frame buffer to transparent.
            clear_rect(*context.frame_buffer, context.images[i - 1]->rect(), Color::Transparent);
        } else if (i > 0 && previous_image_disposal_method == GIFImageDescriptor::DisposalMethod::RestorePrevious) {
            // Previous frame indicated that once disposed, it should be restored to *its* previous
            // underlying image contents, therefore we restore the saved previous frame buffer.
            copy_frame_buffer(*context.frame_buffer, *context.prev_frame_buffer);
        }

        if (image->lzw_min_code_size > 8)
            return Error::from_string_literal("LZW minimum code size is greater than 8");

        auto decoded_stream = TRY(Compress::LzwDecompressor<LittleEndianInputBitStream>::decompress_all(image->lzw_encoded_bytes, image->lzw_min_code_size));

        auto const& color_map = image->use_global_color_map ? context.logical_screen.color_map : image->color_map;

        int pixel_index = 0;
        int row = 0;
        int interlace_pass = 0;

        if (!image->width)
            continue;

        for (auto const& color : decoded_stream.bytes()) {
            auto c = color_map[color];

            int x = pixel_index % image->width + image->x;
            int y = row + image->y;

            if (context.frame_buffer->rect().contains(x, y) && (!image->transparent || color != image->transparency_index)) {
                context.frame_buffer->set_pixel(x, y, c);
            }

            ++pixel_index;
            if (pixel_index % image->width == 0) {
                if (image->interlaced) {
                    if (interlace_pass < 4) {
                        if (row + INTERLACE_ROW_STRIDES[interlace_pass] >= image->height) {
                            ++interlace_pass;
                            if (interlace_pass < 4)
                                row = INTERLACE_ROW_OFFSETS[interlace_pass];
                        } else {
                            row += INTERLACE_ROW_STRIDES[interlace_pass];
                        }
                    }
                } else {
                    ++row;
                }
            }
        }

        context.current_frame = i;
        context.state = GIFLoadingContext::State::FrameComplete;
    }

    return {};
}

static ErrorOr<void> load_header_and_logical_screen(GIFLoadingContext& context)
{
    if (TRY(context.stream.size()) < 32)
        return Error::from_string_literal("Size too short for GIF frame descriptors");

    TRY(decode_gif_header(context.stream));

    context.logical_screen.width = TRY(context.stream.read_value<LittleEndian<u16>>());
    context.logical_screen.height = TRY(context.stream.read_value<LittleEndian<u16>>());

    auto packed_fields = TRY(context.stream.read_value<u8>());
    context.background_color_index = TRY(context.stream.read_value<u8>());
    [[maybe_unused]] auto pixel_aspect_ratio = TRY(context.stream.read_value<u8>());

    // Global Color Table; if the flag is set, the Global Color Table will
    // immediately follow the Logical Screen Descriptor.
    bool global_color_table_flag = packed_fields & 0x80;

    if (global_color_table_flag) {
        u8 bits_per_pixel = (packed_fields & 7) + 1;
        size_t color_map_entry_count = 1 << bits_per_pixel;

        for (size_t i = 0; i < color_map_entry_count; ++i) {
            u8 r = TRY(context.stream.read_value<u8>());
            u8 g = TRY(context.stream.read_value<u8>());
            u8 b = TRY(context.stream.read_value<u8>());
            context.logical_screen.color_map[i] = { r, g, b };
        }
    }

    return {};
}

static ErrorOr<void> load_gif_frame_descriptors(GIFLoadingContext& context)
{
    NonnullOwnPtr<GIFImageDescriptor> current_image = make<GIFImageDescriptor>();
    for (;;) {
        u8 sentinel = TRY(context.stream.read_value<u8>());

        if (sentinel == '!') {
            u8 extension_type = TRY(context.stream.read_value<u8>());

            u8 sub_block_length = 0;

            Vector<u8> sub_block {};
            for (;;) {
                sub_block_length = TRY(context.stream.read_value<u8>());
                if (sub_block_length == 0)
                    break;

                TRY(sub_block.try_resize(sub_block.size() + sub_block_length));
                TRY(context.stream.read_until_filled(sub_block.span().slice_from_end(sub_block_length)));
            }

            if (extension_type == 0xF9) {
                if (sub_block.size() != 4) {
                    dbgln_if(GIF_DEBUG, "Unexpected graphic control size");
                    continue;
                }

                u8 disposal_method = (sub_block[0] & 0x1C) >> 2;
                current_image->disposal_method = (GIFImageDescriptor::DisposalMethod)disposal_method;

                u8 user_input = (sub_block[0] & 0x2) >> 1;
                current_image->user_input = user_input == 1;

                u8 transparent = sub_block[0] & 1;
                current_image->transparent = transparent == 1;

                u16 duration = sub_block[1] + ((u16)sub_block[2] << 8);
                current_image->duration = duration;

                current_image->transparency_index = sub_block[3];

                dbgln_if(GIF_DEBUG, "Graphic control: disposal_method={}, user_input={}, transparent={}, duration={}", (int)current_image->disposal_method, current_image->user_input, current_image->transparent, current_image->duration);
            }

            if (extension_type == 0xFF) {
                if (sub_block.size() != 14) {
                    dbgln_if(GIF_DEBUG, "Unexpected application extension size: {}", sub_block.size());
                    continue;
                }

                if (sub_block[11] != 1) {
                    dbgln_if(GIF_DEBUG, "Unexpected application extension format");
                    continue;
                }

                u16 loops = sub_block[12] + (sub_block[13] << 8);
                context.loops = loops;

                dbgln_if(GIF_DEBUG, "Application extension: loops={}", context.loops);
            }

            continue;
        }

        if (sentinel == ',') {
            context.images.append(move(current_image));
            auto& image = context.images.last();

            image->x = TRY(context.stream.read_value<LittleEndian<u16>>());
            image->y = TRY(context.stream.read_value<LittleEndian<u16>>());
            image->width = TRY(context.stream.read_value<LittleEndian<u16>>());
            image->height = TRY(context.stream.read_value<LittleEndian<u16>>());

            auto packed_fields = TRY(context.stream.read_value<u8>());

            image->use_global_color_map = !(packed_fields & 0x80);
            image->interlaced = (packed_fields & 0x40) != 0;

            dbgln_if(GIF_DEBUG, "Image descriptor: x={}, y={}, width={}, height={}, use_global_color_map={}, local_map_size_exponent={}, interlaced={}", image->x, image->y, image->width, image->height, image->use_global_color_map, (packed_fields & 7) + 1, image->interlaced);

            if (!image->use_global_color_map) {
                size_t local_color_table_size = AK::exp2<size_t>((packed_fields & 7) + 1);

                for (size_t i = 0; i < local_color_table_size; ++i) {
                    u8 r = TRY(context.stream.read_value<u8>());
                    u8 g = TRY(context.stream.read_value<u8>());
                    u8 b = TRY(context.stream.read_value<u8>());
                    image->color_map[i] = { r, g, b };
                }
            }

            image->lzw_min_code_size = TRY(context.stream.read_value<u8>());

            for (;;) {
                auto const lzw_encoded_bytes_expected = TRY(context.stream.read_value<u8>());

                // Block terminator
                if (lzw_encoded_bytes_expected == 0)
                    break;

                auto const lzw_subblock = TRY(image->lzw_encoded_bytes.get_bytes_for_writing(lzw_encoded_bytes_expected));
                TRY(context.stream.read_until_filled(lzw_subblock));
            }

            current_image = make<GIFImageDescriptor>();
            continue;
        }

        if (sentinel == ';') {
            break;
        }

        return Error::from_string_literal("Unexpected sentinel");
    }

    context.state = GIFLoadingContext::State::FrameDescriptorsLoaded;
    return {};
}

GIFImageDecoderPlugin::GIFImageDecoderPlugin(FixedMemoryStream stream)
{
    m_context = make<GIFLoadingContext>(move(stream));
}

GIFImageDecoderPlugin::~GIFImageDecoderPlugin() = default;

IntSize GIFImageDecoderPlugin::size()
{
    return { m_context->logical_screen.width, m_context->logical_screen.height };
}

bool GIFImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };
    return !decode_gif_header(stream).is_error();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> GIFImageDecoderPlugin::create(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) GIFImageDecoderPlugin(move(stream))));
    TRY(load_header_and_logical_screen(*plugin->m_context));
    return plugin;
}

bool GIFImageDecoderPlugin::is_animated()
{
    if (m_context->error_state != GIFLoadingContext::ErrorState::NoError) {
        return false;
    }

    if (m_context->state < GIFLoadingContext::State::FrameDescriptorsLoaded) {
        if (load_gif_frame_descriptors(*m_context).is_error()) {
            m_context->error_state = GIFLoadingContext::ErrorState::FailedToLoadFrameDescriptors;
            return false;
        }
    }

    return m_context->images.size() > 1;
}

size_t GIFImageDecoderPlugin::loop_count()
{
    if (m_context->error_state != GIFLoadingContext::ErrorState::NoError) {
        return 0;
    }

    if (m_context->state < GIFLoadingContext::State::FrameDescriptorsLoaded) {
        if (load_gif_frame_descriptors(*m_context).is_error()) {
            m_context->error_state = GIFLoadingContext::ErrorState::FailedToLoadFrameDescriptors;
            return 0;
        }
    }

    return m_context->loops;
}

size_t GIFImageDecoderPlugin::frame_count()
{
    if (m_context->error_state != GIFLoadingContext::ErrorState::NoError) {
        return 1;
    }

    if (m_context->state < GIFLoadingContext::State::FrameDescriptorsLoaded) {
        if (load_gif_frame_descriptors(*m_context).is_error()) {
            m_context->error_state = GIFLoadingContext::ErrorState::FailedToLoadFrameDescriptors;
            return 1;
        }
    }

    return m_context->images.size();
}

size_t GIFImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

ErrorOr<ImageFrameDescriptor> GIFImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (m_context->error_state >= GIFLoadingContext::ErrorState::FailedToDecodeAnyFrame) {
        return Error::from_string_literal("GIFImageDecoderPlugin: Decoding failed");
    }

    if (m_context->state < GIFLoadingContext::State::FrameDescriptorsLoaded) {
        if (auto result = load_gif_frame_descriptors(*m_context); result.is_error()) {
            m_context->error_state = GIFLoadingContext::ErrorState::FailedToLoadFrameDescriptors;
            return result.release_error();
        }
    }

    if (m_context->error_state == GIFLoadingContext::ErrorState::NoError) {
        if (auto result = decode_frame(*m_context, index); result.is_error()) {
            if (m_context->state < GIFLoadingContext::State::FrameComplete) {
                m_context->error_state = GIFLoadingContext::ErrorState::FailedToDecodeAnyFrame;
                return result.release_error();
            }
            if (auto result = decode_frame(*m_context, 0); result.is_error()) {
                m_context->error_state = GIFLoadingContext::ErrorState::FailedToDecodeAnyFrame;
                return result.release_error();
            }
            m_context->error_state = GIFLoadingContext::ErrorState::FailedToDecodeAllFrames;
        }
    }

    ImageFrameDescriptor frame {};
    frame.image = TRY(m_context->frame_buffer->clone());
    frame.duration = m_context->images[index]->duration * 10;

    if (frame.duration <= 10) {
        frame.duration = 100;
    }

    return frame;
}

}
