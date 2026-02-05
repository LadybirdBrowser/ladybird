/*
 * Copyright (c) 2020, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/BMPWriter.h>

namespace Gfx {

class OutputStreamer {
public:
    OutputStreamer(u8* data)
        : m_data(data)
    {
    }

    void write_u8(u8 i)
    {
        *(m_data++) = i;
    }

    void write_u16(u16 i)
    {
        *(m_data++) = i & 0xFF;
        *(m_data++) = (i >> 8) & 0xFF;
    }

    void write_u32(u32 i)
    {
        write_u16(i & 0xFFFF);
        write_u16((i >> 16) & 0xFFFF);
    }

    void write_i32(i32 i)
    {
        write_u32(static_cast<u32>(i));
    }

private:
    u8* m_data;
};

static ErrorOr<ByteBuffer> write_pixel_data(Bitmap const& bitmap, int pixel_row_data_size, int bytes_per_pixel, bool include_alpha_channel)
{
    int image_size = pixel_row_data_size * bitmap.height();
    auto buffer = TRY(ByteBuffer::create_uninitialized(image_size));

    int current_row = 0;
    for (int y = bitmap.height() - 1; y >= 0; --y) {
        auto* row = buffer.data() + (pixel_row_data_size * current_row++);
        for (int x = 0; x < bitmap.width(); x++) {
            auto pixel = bitmap.get_pixel(x, y);
            row[x * bytes_per_pixel + 0] = pixel.blue();
            row[x * bytes_per_pixel + 1] = pixel.green();
            row[x * bytes_per_pixel + 2] = pixel.red();
            if (include_alpha_channel)
                row[x * bytes_per_pixel + 3] = pixel.alpha();
        }
    }

    return buffer;
}

ErrorOr<ByteBuffer> BMPWriter::encode(Bitmap const& bitmap, Options options)
{
    return BMPWriter().dump(bitmap, options);
}

ByteBuffer BMPWriter::compress_pixel_data(ByteBuffer pixel_data, BMPWriter::Compression compression)
{
    switch (compression) {
    case BMPWriter::Compression::BI_BITFIELDS:
    case BMPWriter::Compression::BI_RGB:
        return pixel_data;
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<ByteBuffer> BMPWriter::dump(Bitmap const& bitmap, Options options)
{
    Options::DibHeader dib_header = options.dib_header;

    auto icc_data = options.icc_data;
    if (icc_data.has_value() && dib_header < Options::DibHeader::V5)
        return Error::from_string_literal("can only embed ICC profiles in v5+ bmps");

    switch (dib_header) {
    case Options::DibHeader::Info:
        m_compression = Compression::BI_RGB;
        m_bytes_per_pixel = 3;
        m_include_alpha_channel = false;
        break;
    case Options::DibHeader::V3:
    case Options::DibHeader::V4:
    case Options::DibHeader::V5:
        m_compression = Compression::BI_BITFIELDS;
        m_bytes_per_pixel = 4;
        m_include_alpha_channel = true;
    }

    size_t const file_header_size = 14;
    size_t header_size = file_header_size + (u32)dib_header;

    int pixel_row_data_size = (m_bytes_per_pixel * 8 * bitmap.width() + 31) / 32 * 4;
    int image_size = pixel_row_data_size * bitmap.height();
    auto buffer = TRY(ByteBuffer::create_uninitialized(header_size));

    auto pixel_data = TRY(write_pixel_data(bitmap, pixel_row_data_size, m_bytes_per_pixel, m_include_alpha_channel));
    pixel_data = compress_pixel_data(move(pixel_data), m_compression);

    size_t icc_profile_size = 0;
    if (icc_data.has_value())
        icc_profile_size = icc_data->size();

    size_t pixel_data_offset = header_size + icc_profile_size;
    size_t file_size = pixel_data_offset + pixel_data.size();
    OutputStreamer streamer(buffer.data());
    streamer.write_u8('B');
    streamer.write_u8('M');
    streamer.write_u32(file_size);
    streamer.write_u32(0);
    streamer.write_u32(pixel_data_offset);

    streamer.write_u32((u32)dib_header);       // Header size
    streamer.write_i32(bitmap.width());        // ImageWidth
    streamer.write_i32(bitmap.height());       // ImageHeight
    streamer.write_u16(1);                     // Planes
    streamer.write_u16(m_bytes_per_pixel * 8); // BitsPerPixel
    streamer.write_u32((u32)m_compression);    // Compression
    streamer.write_u32(image_size);            // ImageSize
    streamer.write_i32(0);                     // XpixelsPerMeter
    streamer.write_i32(0);                     // YpixelsPerMeter
    streamer.write_u32(0);                     // TotalColors
    streamer.write_u32(0);                     // ImportantColors

    if (dib_header >= Options::DibHeader::V3) {
        streamer.write_u32(0x00ff0000); // Red bitmask
        streamer.write_u32(0x0000ff00); // Green bitmask
        streamer.write_u32(0x000000ff); // Blue bitmask
        streamer.write_u32(0xff000000); // Alpha bitmask
    }

    if (dib_header >= Options::DibHeader::V4) {
        if (icc_data.has_value())
            streamer.write_u32(0x4D424544); // Colorspace EMBEDDED
        else
            streamer.write_u32(0); // Colorspace CALIBRATED_RGB

        for (int i = 0; i < 12; i++) {
            streamer.write_u32(0); // Endpoints and gamma
        }
    }

    if (dib_header >= Options::DibHeader::V5) {
        streamer.write_u32(4); // Rendering intent IMAGES / Perceptual.

        if (icc_data.has_value()) {
            streamer.write_u32((u32)dib_header);  // Profile data (relative to file_header_size)
            streamer.write_u32(icc_data->size()); // Profile size
        } else {
            streamer.write_u32(0); // Profile data
            streamer.write_u32(0); // Profile size
        }
        streamer.write_u32(0); // Reserved
    }

    if (icc_data.has_value())
        TRY(buffer.try_append(icc_data.value()));

    TRY(buffer.try_append(pixel_data));
    return buffer;
}

}
