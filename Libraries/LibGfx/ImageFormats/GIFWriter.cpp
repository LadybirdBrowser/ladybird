/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitStream.h>
#include <LibCompress/Lzw.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/GIFWriter.h>
#include <LibGfx/MedianCut.h>

namespace Gfx {

namespace {

ErrorOr<void> write_header(Stream& stream)
{
    // 17. Header
    TRY(stream.write_until_depleted("GIF89a"sv));
    return {};
}

ErrorOr<void> write_logical_descriptor(BigEndianOutputBitStream& stream, IntSize size)
{
    // 18. Logical Screen Descriptor

    if (size.width() > NumericLimits<u16>::max() || size.height() > NumericLimits<u16>::max())
        return Error::from_string_literal("Bitmap size is too big for a GIF");

    TRY(stream.write_value<u16>(size.width()));
    TRY(stream.write_value<u16>(size.height()));

    // Global Color Table Flag
    TRY(stream.write_bits(false, 1));
    // Color Resolution
    TRY(stream.write_bits(6u, 3));
    // Sort Flag
    TRY(stream.write_bits(false, 1));
    // Size of Global Color Table
    TRY(stream.write_bits(0u, 3));

    // Background Color Index
    TRY(stream.write_value<u8>(0));

    // Pixel Aspect Ratio
    // NOTE: We can write a zero as most decoders discard the value.
    TRY(stream.write_value<u8>(0));

    return {};
}

ErrorOr<void> write_color_table(Stream& stream, ColorPalette const& palette)
{
    // 19. Global Color Table or 21. Local Color Table.

    for (u16 i = 0; i < 256; ++i) {
        auto const color = i < palette.palette().size() ? palette.palette()[i] : Color::NamedColor::White;
        TRY(stream.write_value<u8>(color.red()));
        TRY(stream.write_value<u8>(color.green()));
        TRY(stream.write_value<u8>(color.blue()));
    }
    return {};
}

ErrorOr<void> write_image_data(Stream& stream, Bitmap const& bitmap, ColorPalette const& palette)
{
    // 22. Table Based Image Data
    auto const pixel_number = static_cast<u32>(bitmap.width() * bitmap.height());
    auto indexes = TRY(ByteBuffer::create_uninitialized(pixel_number));
    for (u32 i = 0; i < pixel_number; ++i) {
        auto const color = Color::from_argb(*(bitmap.begin() + i));
        indexes[i] = palette.index_of_closest_color(color);
    }

    constexpr u8 lzw_minimum_code_size = 8;
    auto const encoded = TRY(Compress::LzwCompressor::compress_all(move(indexes), lzw_minimum_code_size));

    auto const number_of_subblocks = ceil_div(encoded.size(), 255ul);

    TRY(stream.write_value<u8>(lzw_minimum_code_size));

    for (u32 i = 0; i < number_of_subblocks; ++i) {
        auto const offset = i * 255;
        auto const to_write = min(255, encoded.size() - offset);
        TRY(stream.write_value<u8>(to_write));
        TRY(stream.write_until_depleted(encoded.bytes().slice(offset, to_write)));
    }

    // Block terminator
    TRY(stream.write_value<u8>(0));

    return {};
}

ErrorOr<void> write_image_descriptor(BigEndianOutputBitStream& stream, Bitmap const& bitmap, IntPoint at = {})
{
    // 20. Image Descriptor

    // Image Separator
    TRY(stream.write_value<u8>(0x2c));
    // Image Left Position
    TRY(stream.write_value<u16>(at.x()));
    // Image Top Position
    TRY(stream.write_value<u16>(at.y()));
    // Image Width
    TRY(stream.write_value<u16>(bitmap.width()));
    // Image Height
    TRY(stream.write_value<u16>(bitmap.height()));

    // Local Color Table Flag
    TRY(stream.write_bits(true, 1));
    // Interlace Flag
    TRY(stream.write_bits(false, 1));
    // Sort Flag
    TRY(stream.write_bits(false, 1));
    // Reserved
    TRY(stream.write_bits(0u, 2));
    // Size of Local Color Table
    TRY(stream.write_bits(7u, 3));
    return {};
}

ErrorOr<void> write_graphic_control_extension(BigEndianOutputBitStream& stream, int duration_ms)
{
    // 23. Graphic Control Extension

    // Extension Introducer
    TRY(stream.write_value<u8>(0x21));
    // Graphic Control Label
    TRY(stream.write_value<u8>(0xF9));

    // Block Size
    TRY(stream.write_value<u8>(4));

    // Packed Field
    // Reserved
    TRY(stream.write_bits(0u, 3));
    // Disposal Method
    TRY(stream.write_bits(0u, 3));
    // User Input Flag
    TRY(stream.write_bits(false, 1));
    // Transparency Flag
    TRY(stream.write_bits(false, 1));

    // Delay Time
    TRY(stream.write_value<u16>(duration_ms / 10));

    // Transparent Color Index
    TRY(stream.write_value<u8>(0));

    // Block Terminator
    TRY(stream.write_value<u8>(0));

    return {};
}

ErrorOr<void> write_trailer(Stream& stream)
{
    TRY(stream.write_value<u8>(0x3B));
    return {};
}

class GIFAnimationWriter : public AnimationWriter {
public:
    GIFAnimationWriter(SeekableStream& stream)
        : m_stream(stream)
    {
    }

    virtual ErrorOr<void> add_frame(Bitmap&, int, IntPoint) override;

private:
    SeekableStream& m_stream;
    bool m_is_first_frame { true };
};

ErrorOr<void> GIFAnimationWriter::add_frame(Bitmap& bitmap, int duration_ms, IntPoint at = {})
{
    // Let's get rid of the previously written trailer
    if (!m_is_first_frame)
        TRY(m_stream.seek(-1, SeekMode::FromCurrentPosition));

    m_is_first_frame = false;

    // Write a Table-Based Image
    BigEndianOutputBitStream bit_stream { MaybeOwned { m_stream } };
    TRY(write_graphic_control_extension(bit_stream, duration_ms));
    TRY(write_image_descriptor(bit_stream, bitmap, at));

    auto const palette = TRY(median_cut(bitmap, 256));
    TRY(write_color_table(m_stream, palette));
    TRY(write_image_data(m_stream, bitmap, palette));

    // We always write a trailer to ensure that the file is valid.
    TRY(write_trailer(m_stream));

    return {};
}

ErrorOr<void> write_netscape_extension(BigEndianOutputBitStream& stream, u16 loop_count)
{
    // This is a vendor extension, its sole usage is to provide the loop count.
    // I used this link as a source: https://web.archive.org/web/19990418091037/http://www6.uniovi.es/gifanim/gifabout.htm

    // Extension Introducer
    TRY(stream.write_value<u8>(0x21));
    // Application Extension Label
    TRY(stream.write_value<u8>(0xFF));

    // Block Size
    constexpr auto netscape_signature = "NETSCAPE2.0"sv;
    TRY(stream.write_value<u8>(netscape_signature.length()));
    TRY(stream.write_until_depleted(netscape_signature));

    // Length of Data Sub-Block
    TRY(stream.write_value<u8>(3));

    // Undocumented
    TRY(stream.write_value<u8>(1));

    // Number of loops, 0 means infinite
    TRY(stream.write_value<u16>(loop_count));

    // Block Terminator
    TRY(stream.write_value<u8>(0));

    return {};
}

}

ErrorOr<void> GIFWriter::encode(Stream& stream, Bitmap const& bitmap)
{
    auto const palette = TRY(median_cut(bitmap, 256));
    TRY(write_header(stream));

    BigEndianOutputBitStream bit_stream { MaybeOwned<Stream> { stream } };
    TRY(write_logical_descriptor(bit_stream, bitmap.size()));

    // Write a Table-Based Image
    TRY(write_image_descriptor(bit_stream, bitmap));
    TRY(write_color_table(bit_stream, palette));
    TRY(write_image_data(stream, bitmap, palette));

    TRY(write_trailer(bit_stream));

    return {};
}

ErrorOr<NonnullOwnPtr<AnimationWriter>> GIFWriter::start_encoding_animation(SeekableStream& stream, IntSize dimensions, u16 loop_count)
{
    TRY(write_header(stream));

    BigEndianOutputBitStream bit_stream { MaybeOwned<Stream> { stream } };
    TRY(write_logical_descriptor(bit_stream, dimensions));

    // Vendor extension to support looping
    TRY(write_netscape_extension(bit_stream, loop_count));

    return make<GIFAnimationWriter>(stream);
}

}
