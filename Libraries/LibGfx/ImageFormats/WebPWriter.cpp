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
#include <LibGfx/ImageFormats/AnimationWriter.h>
#include <LibGfx/ImageFormats/WebPShared.h>
#include <LibGfx/ImageFormats/WebPWriter.h>

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

// FIXME: Consider using LibRIFF for RIFF writing details. (It currently has no writing support.)
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

// FIXME: Consider using LibRIFF for RIFF writing details. (It currently has no writing support.)
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

class WebPAnimationWriter : public AnimationWriter {
public:
    WebPAnimationWriter(SeekableStream& stream, IntSize dimensions, u8 original_vp8x_flags, VP8LEncoderOptions vp8l_options)
        : m_stream(stream)
        , m_dimensions(dimensions)
        , m_vp8x_flags(original_vp8x_flags)
        , m_vp8l_options(vp8l_options)
    {
    }

    virtual ErrorOr<void> add_frame(Bitmap&, int, IntPoint) override;

    ErrorOr<void> update_size_in_header();
    ErrorOr<void> set_alpha_bit_in_header();

private:
    SeekableStream& m_stream;
    IntSize m_dimensions;
    u8 m_vp8x_flags { 0 };
    VP8LEncoderOptions m_vp8l_options;
};

static ErrorOr<void> align_to_two(SeekableStream& stream)
{
    return align_to_two(stream, TRY(stream.tell()));
}

static ErrorOr<void> write_ANMF_chunk_header(Stream& stream, ANMFChunkHeader const& chunk, size_t payload_size)
{
    if (chunk.frame_width > (1 << 24) || chunk.frame_height > (1 << 24))
        return Error::from_string_literal("WebP dimensions too large for ANMF chunk");

    if (chunk.frame_width == 0 || chunk.frame_height == 0)
        return Error::from_string_literal("WebP lossless animation frames must be at least one pixel wide and tall");

    if (chunk.frame_x % 2 != 0 || chunk.frame_y % 2 != 0)
        return Error::from_string_literal("WebP lossless animation frames must be at at even coordinates");

    dbgln_if(WEBP_DEBUG, "writing ANMF frame_x {} frame_y {} frame_width {} frame_height {} frame_duration {} blending_method {} disposal_method {}",
        chunk.frame_x, chunk.frame_y, chunk.frame_width, chunk.frame_height, chunk.frame_duration_in_milliseconds, (int)chunk.blending_method, (int)chunk.disposal_method);

    TRY(write_chunk_header(stream, "ANMF"sv, 16 + payload_size));

    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // "Frame X: 24 bits (uint24)
    //  The X coordinate of the upper left corner of the frame is Frame X * 2."
    TRY(bit_stream.write_bits(chunk.frame_x / 2, 24u));

    // "Frame Y: 24 bits (uint24)
    //  The Y coordinate of the upper left corner of the frame is Frame Y * 2."
    TRY(bit_stream.write_bits(chunk.frame_y / 2, 24u));

    // "Frame Width: 24 bits (uint24)
    //  The 1-based width of the frame. The frame width is 1 + Frame Width Minus One."
    TRY(bit_stream.write_bits(chunk.frame_width - 1, 24u));

    // "Frame Height: 24 bits (uint24)
    //  The 1-based height of the frame. The frame height is 1 + Frame Height Minus One."
    TRY(bit_stream.write_bits(chunk.frame_height - 1, 24u));

    // "Frame Duration: 24 bits (uint24)"
    TRY(bit_stream.write_bits(chunk.frame_duration_in_milliseconds, 24u));

    // Don't use bit_stream.write_bits() to write individual flags here:
    // The spec describes bit flags in MSB to LSB order, but write_bits() writes LSB to MSB.
    u8 flags = 0;
    // "Reserved: 6 bits
    //  MUST be 0. Readers MUST ignore this field."

    // "Blending method (B): 1 bit"
    if (chunk.blending_method == ANMFChunkHeader::BlendingMethod::DoNotBlend)
        flags |= 0x2;

    // "Disposal method (D): 1 bit"
    if (chunk.disposal_method == ANMFChunkHeader::DisposalMethod::DisposeToBackgroundColor)
        flags |= 0x1;

    TRY(bit_stream.write_bits(flags, 8u));

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.flush_buffer_to_stream());

    return {};
}

ErrorOr<void> WebPAnimationWriter::add_frame(Bitmap& bitmap, int duration_ms, IntPoint at)
{
    if (at.x() < 0 || at.y() < 0 || at.x() + bitmap.width() > m_dimensions.width() || at.y() + bitmap.height() > m_dimensions.height())
        return Error::from_string_literal("Frame does not fit in animation dimensions");

    // Since we have a SeekableStream, we could write both the VP8L chunk header and the ANMF chunk header with a placeholder size,
    // compress the frame data directly to the stream, and then go back and update the two sizes.
    // That's pretty messy though, and the compressed image data is smaller than the uncompressed bitmap passed in. So we'll buffer it.
    bool is_fully_opaque;
    auto vp8l_data_bytes = TRY(compress_VP8L_image_data(bitmap, m_vp8l_options, is_fully_opaque));

    ANMFChunkHeader chunk;
    chunk.frame_x = static_cast<u32>(at.x());
    chunk.frame_y = static_cast<u32>(at.y());
    chunk.frame_width = static_cast<u32>(bitmap.width());
    chunk.frame_height = static_cast<u32>(bitmap.height());
    chunk.frame_duration_in_milliseconds = static_cast<u32>(duration_ms);
    chunk.blending_method = ANMFChunkHeader::BlendingMethod::DoNotBlend;
    chunk.disposal_method = ANMFChunkHeader::DisposalMethod::DoNotDispose;

    TRY(write_ANMF_chunk_header(m_stream, chunk, compute_VP8L_chunk_size(vp8l_data_bytes)));
    bool alpha_is_used_hint = !is_fully_opaque;
    TRY(write_VP8L_chunk(m_stream, bitmap.width(), bitmap.height(), alpha_is_used_hint, vp8l_data_bytes));

    TRY(update_size_in_header());

    if (!(m_vp8x_flags & 0x10) && !is_fully_opaque)
        TRY(set_alpha_bit_in_header());

    return {};
}

ErrorOr<void> WebPAnimationWriter::update_size_in_header()
{
    auto current_offset = TRY(m_stream.tell());
    TRY(m_stream.seek(4, SeekMode::SetPosition));
    VERIFY(current_offset > 8);
    TRY(m_stream.write_value<LittleEndian<u32>>(current_offset - 8));
    TRY(m_stream.seek(current_offset, SeekMode::SetPosition));
    return {};
}

ErrorOr<void> WebPAnimationWriter::set_alpha_bit_in_header()
{
    m_vp8x_flags |= 0x10;

    auto current_offset = TRY(m_stream.tell());
    // 4 bytes for "RIFF",
    // 4 bytes RIFF chunk size (i.e. file size - 8),
    // 4 bytes for "WEBP",
    // 4 bytes for "VP8X",
    // 4 bytes for VP8X chunk size,
    // followed by VP8X flags in the first byte of the VP8X chunk data.
    TRY(m_stream.seek(20, SeekMode::SetPosition));
    TRY(m_stream.write_value<u8>(m_vp8x_flags));
    TRY(m_stream.seek(current_offset, SeekMode::SetPosition));
    return {};
}

static ErrorOr<void> write_ANIM_chunk(Stream& stream, ANIMChunk const& chunk)
{
    TRY(write_chunk_header(stream, "ANIM"sv, 6)); // Size of the ANIM chunk.
    TRY(stream.write_value<LittleEndian<u32>>(chunk.background_color));
    TRY(stream.write_value<LittleEndian<u16>>(chunk.loop_count));
    return {};
}

ErrorOr<NonnullOwnPtr<AnimationWriter>> WebPWriter::start_encoding_animation(SeekableStream& stream, IntSize dimensions, int loop_count, Color background_color, Options const& options)
{
    // We'll update the stream with the actual size later.
    TRY(write_webp_header(stream, 0));

    VP8XHeader vp8x_header;
    vp8x_header.has_icc = options.icc_data.has_value();
    vp8x_header.width = dimensions.width();
    vp8x_header.height = dimensions.height();
    vp8x_header.has_animation = true;
    TRY(write_VP8X_chunk(stream, vp8x_header));
    VERIFY(TRY(stream.tell()) % 2 == 0);

    ByteBuffer iccp_chunk_bytes;
    if (options.icc_data.has_value()) {
        TRY(write_chunk_header(stream, "ICCP"sv, options.icc_data.value().size()));
        TRY(stream.write_until_depleted(options.icc_data.value()));
        TRY(align_to_two(stream));
    }

    TRY(write_ANIM_chunk(stream, { .background_color = background_color.value(), .loop_count = static_cast<u16>(loop_count) }));

    auto writer = make<WebPAnimationWriter>(stream, dimensions, vp8x_flags_from_header(vp8x_header), options.vp8l_options);
    TRY(writer->update_size_in_header());
    return writer;
}

}
