/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Container: https://developers.google.com/speed/webp/docs/riff_container

#include <AK/BitStream.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/WebPShared.h>
#include <LibGfx/ImageFormats/WebPWriter.h>
#include <LibGfx/ImageFormats/WebPWriterLossless.h>

namespace Gfx {

// https://developers.google.com/speed/webp/docs/riff_container#webp_file_header
static ErrorOr<void> write_webp_header(Stream& stream, unsigned data_size)
{
    TRY(stream.write_until_depleted("RIFF"sv));
    TRY(stream.write_value<LittleEndian<u32>>("WEBP"sv.length() + data_size));
    TRY(stream.write_until_depleted("WEBP"sv));
    return {};
}

static ErrorOr<void> write_chunk_header(Stream& stream, StringView chunk_fourcc, unsigned data_size)
{
    TRY(stream.write_until_depleted(chunk_fourcc));
    TRY(stream.write_value<LittleEndian<u32>>(data_size));
    return {};
}

// https://developers.google.com/speed/webp/docs/riff_container#simple_file_format_lossless
// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#7_overall_structure_of_the_format
static ErrorOr<void> write_VP8L_header(Stream& stream, unsigned width, unsigned height, bool alpha_is_used_hint)
{
    // "The 14-bit precision for image width and height limits the maximum size of a WebP lossless image to 16384✕16384 pixels."
    if (width > 16384 || height > 16384)
        return Error::from_string_literal("WebP lossless images can't be larger than 16384x16384 pixels");

    if (width == 0 || height == 0)
        return Error::from_string_literal("WebP lossless images must be at least one pixel wide and tall");

    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // Signature byte.
    TRY(bit_stream.write_bits(0x2fu, 8u)); // Signature byte

    // 14 bits width-1, 14 bits height-1, 1 bit alpha hint, 3 bit version_number.
    TRY(bit_stream.write_bits(width - 1, 14u));
    TRY(bit_stream.write_bits(height - 1, 14u));

    // "The alpha_is_used bit is a hint only, and should not impact decoding.
    //  It should be set to 0 when all alpha values are 255 in the picture, and 1 otherwise."
    TRY(bit_stream.write_bits(alpha_is_used_hint, 1u));

    // "The version_number is a 3 bit code that must be set to 0."
    TRY(bit_stream.write_bits(0u, 3u));

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.flush_buffer_to_stream());

    return {};
}

static ErrorOr<void> align_to_two(Stream& stream, size_t number_of_bytes_written)
{
    // https://developers.google.com/speed/webp/docs/riff_container
    // "If Chunk Size is odd, a single padding byte -- which MUST be 0 to conform with RIFF -- is added."
    if (number_of_bytes_written % 2 != 0)
        TRY(stream.write_value<u8>(0));
    return {};
}

constexpr size_t vp8l_header_size = 5; // 1 byte signature + (2 * 14 bits width and height + 1 bit alpha hint + 3 bit version_number)

static size_t compute_VP8L_chunk_size(ByteBuffer const& data)
{
    constexpr size_t chunk_header_size = 8; // "VP8L" + size
    return chunk_header_size + align_up_to(vp8l_header_size + data.size(), 2);
}

static ErrorOr<void> write_VP8L_chunk(Stream& stream, unsigned width, unsigned height, bool alpha_is_used_hint, ByteBuffer const& data)
{
    size_t const number_of_bytes_written = vp8l_header_size + data.size();
    TRY(write_chunk_header(stream, "VP8L"sv, number_of_bytes_written));
    TRY(write_VP8L_header(stream, width, height, alpha_is_used_hint));
    TRY(stream.write_until_depleted(data));
    TRY(align_to_two(stream, number_of_bytes_written));
    return {};
}

static u8 vp8x_flags_from_header(VP8XHeader const& header)
{
    u8 flags = 0;

    // "Reserved (Rsv): 2 bits
    //  MUST be 0. Readers MUST ignore this field."

    // "ICC profile (I): 1 bit
    //  Set if the file contains an 'ICCP' Chunk."
    if (header.has_icc)
        flags |= 0x20;

    // "Alpha (L): 1 bit
    //  Set if any of the frames of the image contain transparency information ("alpha")."
    if (header.has_alpha)
        flags |= 0x10;

    // "Exif metadata (E): 1 bit
    //  Set if the file contains Exif metadata."
    if (header.has_exif)
        flags |= 0x8;

    // "XMP metadata (X): 1 bit
    //  Set if the file contains XMP metadata."
    if (header.has_xmp)
        flags |= 0x4;

    // "Animation (A): 1 bit
    //  Set if this is an animated image. Data in 'ANIM' and 'ANMF' Chunks should be used to control the animation."
    if (header.has_animation)
        flags |= 0x2;

    // "Reserved (R): 1 bit
    //  MUST be 0. Readers MUST ignore this field."

    return flags;
}

// https://developers.google.com/speed/webp/docs/riff_container#extended_file_format
static ErrorOr<void> write_VP8X_chunk(Stream& stream, VP8XHeader const& header)
{
    if (header.width > (1 << 24) || header.height > (1 << 24))
        return Error::from_string_literal("WebP dimensions too large for VP8X chunk");

    if (header.width == 0 || header.height == 0)
        return Error::from_string_literal("WebP lossless images must be at least one pixel wide and tall");

    // "The product of Canvas Width and Canvas Height MUST be at most 2^32 - 1."
    u64 product = static_cast<u64>(header.width) * static_cast<u64>(header.height);
    if (product >= (1ull << 32))
        return Error::from_string_literal("WebP dimensions too large for VP8X chunk");

    TRY(write_chunk_header(stream, "VP8X"sv, 10));

    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // Don't use bit_stream.write_bits() to write individual flags here:
    // The spec describes bit flags in MSB to LSB order, but write_bits() writes LSB to MSB.
    TRY(bit_stream.write_bits(vp8x_flags_from_header(header), 8u));

    // "Reserved: 24 bits
    //  MUST be 0. Readers MUST ignore this field."
    TRY(bit_stream.write_bits(0u, 24u));

    // "Canvas Width Minus One: 24 bits
    //  1-based width of the canvas in pixels. The actual canvas width is 1 + Canvas Width Minus One."
    TRY(bit_stream.write_bits(header.width - 1, 24u));

    // "Canvas Height Minus One: 24 bits
    //  1-based height of the canvas in pixels. The actual canvas height is 1 + Canvas Height Minus One."
    TRY(bit_stream.write_bits(header.height - 1, 24u));

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.flush_buffer_to_stream());

    return {};
}

static ErrorOr<void> align_to_two(AllocatingMemoryStream& stream)
{
    return align_to_two(stream, stream.used_buffer_size());
}

ErrorOr<void> WebPWriter::encode(Stream& stream, Bitmap const& bitmap, Options const& options)
{
    // The chunk headers need to know their size, so we either need a SeekableStream or need to buffer the data. We're doing the latter.
    bool is_fully_opaque;
    auto vp8l_data_bytes = TRY(compress_VP8L_image_data(bitmap, options.vp8l_options, is_fully_opaque));
    bool alpha_is_used_hint = !is_fully_opaque;
    dbgln_if(WEBP_DEBUG, "Writing WebP of size {} with alpha hint: {}", bitmap.size(), alpha_is_used_hint);

    ByteBuffer vp8x_chunk_bytes;
    ByteBuffer iccp_chunk_bytes;
    if (options.icc_data.has_value()) {
        // FIXME: The whole writing-and-reading-into-buffer over-and-over is awkward and inefficient.
        //        Maybe add an abstraction that knows its size and can write its data later. This would
        //        allow saving a few copies.
        dbgln_if(WEBP_DEBUG, "Writing VP8X and ICCP chunks.");
        AllocatingMemoryStream iccp_chunk_stream;
        TRY(write_chunk_header(iccp_chunk_stream, "ICCP"sv, options.icc_data.value().size()));
        TRY(iccp_chunk_stream.write_until_depleted(options.icc_data.value()));
        TRY(align_to_two(iccp_chunk_stream));
        iccp_chunk_bytes = TRY(iccp_chunk_stream.read_until_eof());

        AllocatingMemoryStream vp8x_chunk_stream;
        TRY(write_VP8X_chunk(vp8x_chunk_stream, { .has_icc = true, .has_alpha = alpha_is_used_hint, .width = (u32)bitmap.width(), .height = (u32)bitmap.height() }));
        VERIFY(vp8x_chunk_stream.used_buffer_size() % 2 == 0);
        vp8x_chunk_bytes = TRY(vp8x_chunk_stream.read_until_eof());
    }

    u32 total_size = vp8x_chunk_bytes.size() + iccp_chunk_bytes.size() + compute_VP8L_chunk_size(vp8l_data_bytes);
    TRY(write_webp_header(stream, total_size));
    TRY(stream.write_until_depleted(vp8x_chunk_bytes));
    TRY(stream.write_until_depleted(iccp_chunk_bytes));
    TRY(write_VP8L_chunk(stream, bitmap.width(), bitmap.height(), alpha_is_used_hint, vp8l_data_bytes));
    return {};
}

}
