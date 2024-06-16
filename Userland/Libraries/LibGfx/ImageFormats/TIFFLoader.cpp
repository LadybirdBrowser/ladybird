/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TIFFLoader.h"
#include <AK/ConstrainedStream.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/String.h>
#include <LibCompress/Lzw.h>
#include <LibCompress/PackBitsDecoder.h>
#include <LibCompress/Zlib.h>
#include <LibGfx/CMYKBitmap.h>
#include <LibGfx/ImageFormats/CCITTDecoder.h>
#include <LibGfx/ImageFormats/ExifOrientedBitmap.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>

namespace Gfx {

namespace {

CCITT::Group3Options parse_t4_options(u32 bit_field)
{
    // Section 11: CCITT Bilevel Encodings
    CCITT::Group3Options options {};

    if (bit_field & 0b001)
        options.dimensions = CCITT::Group3Options::Mode::TwoDimensions;
    if (bit_field & 0b010)
        options.compression = CCITT::Group3Options::Compression::Uncompressed;
    if (bit_field & 0b100)
        options.use_fill_bits = CCITT::Group3Options::UseFillBits::Yes;

    return options;
}

bool is_bilevel(TIFF::PhotometricInterpretation interpretation)
{
    return interpretation == TIFF::PhotometricInterpretation::WhiteIsZero || interpretation == TIFF::PhotometricInterpretation::BlackIsZero;
}

}

namespace TIFF {

class TIFFLoadingContext {
public:
    enum class State {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
        FrameDecoded,
    };

    TIFFLoadingContext(NonnullOwnPtr<FixedMemoryStream> stream)
        : m_stream(move(stream))
    {
    }

    ErrorOr<void> decode_image_header()
    {
        TRY(read_image_file_header());
        TRY(read_next_image_file_directory());

        m_state = State::HeaderDecoded;
        return {};
    }

    ErrorOr<void> ensure_conditional_tags_are_correct() const
    {
        if (m_metadata.photometric_interpretation() == PhotometricInterpretation::RGBPalette && !m_metadata.color_map().has_value())
            return Error::from_string_literal("TIFFImageDecoderPlugin: RGBPalette image doesn't contain a color map");

        if (m_metadata.tile_width() == 0u || m_metadata.tile_length() == 0u)
            return Error::from_string_literal("TIFFImageDecoderPlugin: Null value in tile's dimensions");

        return {};
    }

    Optional<Vector<u32>> segment_offsets() const
    {
        return m_metadata.strip_offsets().has_value() ? m_metadata.strip_offsets() : m_metadata.tile_offsets();
    }

    Optional<Vector<u32>> segment_byte_counts() const
    {
        return m_metadata.strip_byte_counts().has_value() ? m_metadata.strip_byte_counts() : m_metadata.tile_byte_counts();
    }

    bool is_tiled() const
    {
        return m_metadata.tile_width().has_value() && m_metadata.tile_length().has_value();
    }

    ErrorOr<void> ensure_baseline_tags_are_correct() const
    {
        if (!segment_offsets().has_value())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Missing Offsets tag");

        if (!segment_byte_counts().has_value())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Missing ByteCounts tag");

        if (segment_offsets()->size() != segment_byte_counts()->size())
            return Error::from_string_literal("TIFFImageDecoderPlugin: StripsOffset and StripByteCount have different sizes");

        if (!m_metadata.rows_per_strip().has_value() && segment_byte_counts()->size() != 1 && !is_tiled())
            return Error::from_string_literal("TIFFImageDecoderPlugin: RowsPerStrip is not provided and impossible to deduce");

        if (!is_bilevel(*m_metadata.photometric_interpretation())) {
            if (!m_metadata.bits_per_sample().has_value())
                return Error::from_string_literal("TIFFImageDecoderPlugin: Tag BitsPerSample is missing");

            if (!m_metadata.samples_per_pixel().has_value())
                return Error::from_string_literal("TIFFImageDecoderPlugin: Tag SamplesPerPixel is missing");

            if (any_of(*m_metadata.bits_per_sample(), [](auto bit_depth) { return bit_depth == 0 || bit_depth > 32; }))
                return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid value in BitsPerSample");

            if (m_metadata.bits_per_sample()->size() != m_metadata.samples_per_pixel())
                return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid number of values in BitsPerSample");

            if (*m_metadata.samples_per_pixel() < samples_for_photometric_interpretation())
                return Error::from_string_literal("TIFFImageDecoderPlugin: Not enough values in BitsPerSample for given PhotometricInterpretation");
        }

        return {};
    }

    void cache_values()
    {
        if (m_metadata.photometric_interpretation().has_value())
            m_photometric_interpretation = m_metadata.photometric_interpretation().value();
        if (m_metadata.bits_per_sample().has_value())
            m_bits_per_sample = m_metadata.bits_per_sample().value();
        else if (is_bilevel(m_photometric_interpretation))
            m_bits_per_sample.append(1);
        if (m_metadata.image_width().has_value())
            m_image_width = m_metadata.image_width().value();
        if (m_metadata.predictor().has_value())
            m_predictor = m_metadata.predictor().value();
        m_alpha_channel_index = alpha_channel_index();
    }

    ErrorOr<void> decode_frame()
    {
        TRY(ensure_baseline_tags_are_present(m_metadata));
        TRY(ensure_baseline_tags_are_correct());
        TRY(ensure_conditional_tags_are_correct());
        cache_values();
        auto maybe_error = decode_frame_impl();

        if (maybe_error.is_error()) {
            m_state = State::Error;
            return maybe_error.release_error();
        }

        return {};
    }

    IntSize size() const
    {
        return ExifOrientedBitmap::oriented_size({ *m_metadata.image_width(), *m_metadata.image_length() }, *m_metadata.orientation());
    }

    ExifMetadata const& metadata() const
    {
        return m_metadata;
    }

    State state() const
    {
        return m_state;
    }

    RefPtr<CMYKBitmap> cmyk_bitmap() const
    {
        return m_cmyk_bitmap;
    }

    RefPtr<Bitmap> bitmap() const
    {
        return m_bitmap;
    }

private:
    enum class ByteOrder {
        LittleEndian,
        BigEndian,
    };

    static ErrorOr<u8> read_component(BigEndianInputBitStream& stream, u8 bits)
    {
        // FIXME: This function truncates everything to 8-bits
        auto const value = TRY(stream.read_bits<u32>(bits));

        if (bits > 8)
            return value >> (bits - 8);
        return NumericLimits<u8>::max() * value / ((1 << bits) - 1);
    }

    u8 samples_for_photometric_interpretation() const
    {
        switch (m_photometric_interpretation) {
        case PhotometricInterpretation::WhiteIsZero:
        case PhotometricInterpretation::BlackIsZero:
        case PhotometricInterpretation::RGBPalette:
            return 1;
        case PhotometricInterpretation::RGB:
            return 3;
        case PhotometricInterpretation::CMYK:
            return 4;
        default:
            TODO();
        }
    }

    Optional<u8> alpha_channel_index() const
    {
        if (m_metadata.extra_samples().has_value()) {
            auto const extra_samples = m_metadata.extra_samples().value();
            for (u8 i = 0; i < extra_samples.size(); ++i) {
                if (extra_samples[i] == ExtraSample::UnassociatedAlpha)
                    return i + samples_for_photometric_interpretation();
            }
        }
        return OptionalNone {};
    }

    ErrorOr<u8> manage_extra_channels(BigEndianInputBitStream& stream) const
    {
        // Section 7: Additional Baseline TIFF Requirements
        // Some TIFF files may have more components per pixel than you think. A Baseline TIFF reader must skip over
        // them gracefully, using the values of the SamplesPerPixel and BitsPerSample fields.

        // Both unknown and alpha channels are considered as extra channels, so let's iterate over
        // them, conserve the alpha value (if any) and discard everything else.

        auto const number_base_channels = samples_for_photometric_interpretation();

        Optional<u8> alpha {};

        for (u8 i = number_base_channels; i < m_bits_per_sample.size(); ++i) {
            if (m_alpha_channel_index == i)
                alpha = TRY(read_component(stream, m_bits_per_sample[i]));
            else
                TRY(read_component(stream, m_bits_per_sample[i]));
        }

        return alpha.value_or(NumericLimits<u8>::max());
    }

    ErrorOr<Color> read_color(BigEndianInputBitStream& stream)
    {
        if (m_photometric_interpretation == PhotometricInterpretation::RGB) {
            auto const first_component = TRY(read_component(stream, m_bits_per_sample[0]));
            auto const second_component = TRY(read_component(stream, m_bits_per_sample[1]));
            auto const third_component = TRY(read_component(stream, m_bits_per_sample[2]));

            auto const alpha = TRY(manage_extra_channels(stream));
            return Color(first_component, second_component, third_component, alpha);
        }

        if (m_photometric_interpretation == PhotometricInterpretation::RGBPalette) {
            auto const index = TRY(stream.read_bits<u16>(m_bits_per_sample[0]));
            auto const alpha = TRY(manage_extra_channels(stream));

            // SamplesPerPixel == 1 is a requirement for RGBPalette
            // From description of PhotometricInterpretation in Section 8: Baseline Field Reference Guide
            // "In a TIFF ColorMap, all the Red values come first, followed by the Green values,
            //  then the Blue values."
            u64 const size = 1ul << m_bits_per_sample[0];
            u64 const red_offset = 0 * size;
            u64 const green_offset = 1 * size;
            u64 const blue_offset = 2 * size;

            auto const color_map = *m_metadata.color_map();

            if (blue_offset + index >= color_map.size())
                return Error::from_string_literal("TIFFImageDecoderPlugin: Color index is out of range");

            // FIXME: ColorMap's values are always 16-bits, stop truncating them when we support 16 bits bitmaps
            return Color(
                color_map[red_offset + index] >> 8,
                color_map[green_offset + index] >> 8,
                color_map[blue_offset + index] >> 8,
                alpha);
        }

        if (m_photometric_interpretation == PhotometricInterpretation::WhiteIsZero
            || m_photometric_interpretation == PhotometricInterpretation::BlackIsZero) {
            auto luminosity = TRY(read_component(stream, m_bits_per_sample[0]));

            if (m_photometric_interpretation == PhotometricInterpretation::WhiteIsZero)
                luminosity = ~luminosity;

            auto const alpha = TRY(manage_extra_channels(stream));
            return Color(luminosity, luminosity, luminosity, alpha);
        }

        return Error::from_string_literal("Unsupported value for PhotometricInterpretation");
    }

    ErrorOr<CMYK> read_color_cmyk(BigEndianInputBitStream& stream)
    {
        VERIFY(m_photometric_interpretation == PhotometricInterpretation::CMYK);

        auto const first_component = TRY(read_component(stream, m_bits_per_sample[0]));
        auto const second_component = TRY(read_component(stream, m_bits_per_sample[1]));
        auto const third_component = TRY(read_component(stream, m_bits_per_sample[2]));
        auto const fourth_component = TRY(read_component(stream, m_bits_per_sample[3]));

        // FIXME: We probably won't encounter CMYK images with an alpha channel, but if
        //        we do: the first step to support them is not dropping the value here!
        [[maybe_unused]] auto const alpha = TRY(manage_extra_channels(stream));
        return CMYK { first_component, second_component, third_component, fourth_component };
    }

    template<CallableAs<ErrorOr<ReadonlyBytes>, u32, IntSize> SegmentDecoder>
    ErrorOr<void> loop_over_pixels(SegmentDecoder&& segment_decoder)
    {
        auto const offsets = *segment_offsets();
        auto const byte_counts = *segment_byte_counts();

        auto const segment_length = m_metadata.tile_length().value_or(m_metadata.rows_per_strip().value_or(*m_metadata.image_length()));
        auto const segment_width = m_metadata.tile_width().value_or(m_image_width);
        auto const segment_per_rows = m_metadata.tile_width().map([&](u32 w) { return ceil_div(m_image_width, w); }).value_or(1);

        Variant<ExifOrientedBitmap, ExifOrientedCMYKBitmap> oriented_bitmap = TRY(([&]() -> ErrorOr<Variant<ExifOrientedBitmap, ExifOrientedCMYKBitmap>> {
            if (m_photometric_interpretation == PhotometricInterpretation::CMYK)
                return ExifOrientedCMYKBitmap::create(*metadata().orientation(), { m_image_width, *metadata().image_length() });
            return ExifOrientedBitmap::create(*metadata().orientation(), { m_image_width, *metadata().image_length() }, BitmapFormat::BGRA8888);
        }()));

        for (u32 segment_index = 0; segment_index < offsets.size(); ++segment_index) {
            TRY(m_stream->seek(offsets[segment_index]));

            auto const rows_in_segment = segment_index < offsets.size() - 1 ? segment_length : *m_metadata.image_length() - segment_length * segment_index;
            auto const decoded_bytes = TRY(segment_decoder(byte_counts[segment_index], { segment_width, rows_in_segment }));
            auto decoded_segment = make<FixedMemoryStream>(decoded_bytes);
            auto decoded_stream = make<BigEndianInputBitStream>(move(decoded_segment));

            for (u32 row = 0; row < segment_length; row++) {
                auto const image_row = row + segment_length * (segment_index / segment_per_rows);
                if (image_row >= *m_metadata.image_length())
                    break;

                Optional<Color> last_color {};

                for (u32 column = 0; column < segment_width; ++column) {
                    // If image_length % segment_length != 0, the last tile will be padded.
                    // This variable helps us to skip these last columns. Note that we still
                    // need to read the sample from the stream.
                    auto const image_column = column + segment_width * (segment_index % segment_per_rows);

                    if (m_photometric_interpretation == PhotometricInterpretation::CMYK) {
                        auto const cmyk = TRY(read_color_cmyk(*decoded_stream));
                        if (image_column >= m_image_width)
                            continue;
                        oriented_bitmap.get<ExifOrientedCMYKBitmap>().set_pixel(image_column, image_row, cmyk);
                    } else {
                        auto color = TRY(read_color(*decoded_stream));

                        // FIXME:  We should do the differencing at the byte-stream level, that would make it
                        //         compatible with both LibPDF and all color formats.
                        if (m_predictor == Predictor::HorizontalDifferencing && last_color.has_value()) {
                            color.set_red(last_color->red() + color.red());
                            color.set_green(last_color->green() + color.green());
                            color.set_blue(last_color->blue() + color.blue());
                            if (m_alpha_channel_index.has_value())
                                color.set_alpha(last_color->alpha() + color.alpha());
                        }

                        last_color = color;
                        if (image_column >= m_image_width)
                            continue;
                        oriented_bitmap.get<ExifOrientedBitmap>().set_pixel(image_column, image_row, color.value());
                    }
                }

                decoded_stream->align_to_byte_boundary();
            }
        }

        if (m_photometric_interpretation == PhotometricInterpretation::CMYK)
            m_cmyk_bitmap = oriented_bitmap.get<ExifOrientedCMYKBitmap>().bitmap();
        else
            m_bitmap = oriented_bitmap.get<ExifOrientedBitmap>().bitmap();

        return {};
    }

    ErrorOr<void> ensure_tags_are_correct_for_ccitt() const
    {
        // Section 8: Baseline Field Reference Guide
        // BitsPerSample must be 1, since this type of compression is defined only for bilevel images.
        if (m_bits_per_sample.size() > 1)
            return Error::from_string_literal("TIFFImageDecoderPlugin: CCITT image with BitsPerSample greater than one");
        if (!is_bilevel(*m_metadata.photometric_interpretation()))
            return Error::from_string_literal("TIFFImageDecoderPlugin: CCITT compression is used on a non bilevel image");

        return {};
    }

    ErrorOr<ByteBuffer> read_bytes_considering_fill_order(u32 bytes_to_read) const
    {
        auto const reverse_byte = [](u8 b) {
            b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
            b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
            b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
            return b;
        };

        auto const bytes = TRY(m_stream->read_in_place<u8 const>(bytes_to_read));
        auto copy = TRY(ByteBuffer::copy(bytes));
        if (m_metadata.fill_order() == FillOrder::RightToLeft) {
            for (auto& byte : copy.bytes())
                byte = reverse_byte(byte);
        }

        return copy;
    }

    ErrorOr<void> decode_frame_impl()
    {
        switch (*m_metadata.compression()) {
        case Compression::NoCompression: {
            auto identity = [&](u32 num_bytes, IntSize) {
                return m_stream->read_in_place<u8 const>(num_bytes);
            };

            TRY(loop_over_pixels(move(identity)));
            break;
        }
        case Compression::CCITTRLE: {
            TRY(ensure_tags_are_correct_for_ccitt());

            ByteBuffer decoded_bytes {};
            auto decode_ccitt_rle_segment = [&](u32 num_bytes, IntSize segment_size) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(read_bytes_considering_fill_order(num_bytes));
                decoded_bytes = TRY(CCITT::decode_ccitt_rle(encoded_bytes, segment_size.width(), segment_size.height()));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_ccitt_rle_segment)));
            break;
        }
        case Compression::Group3Fax: {
            TRY(ensure_tags_are_correct_for_ccitt());

            auto const parameters = parse_t4_options(*m_metadata.t4_options());
            ByteBuffer decoded_bytes {};
            auto decode_group3_segment = [&](u32 num_bytes, IntSize segment_size) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(read_bytes_considering_fill_order(num_bytes));
                decoded_bytes = TRY(CCITT::decode_ccitt_group3(encoded_bytes, segment_size.width(), segment_size.height(), parameters));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_group3_segment)));
            break;
        }
        case Compression::Group4Fax: {
            TRY(ensure_tags_are_correct_for_ccitt());

            // FIXME: We need to parse T6 options
            ByteBuffer decoded_bytes {};
            auto decode_group3_segment = [&](u32 num_bytes, IntSize segment_size) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(read_bytes_considering_fill_order(num_bytes));
                decoded_bytes = TRY(CCITT::decode_ccitt_group4(encoded_bytes, segment_size.width(), segment_size.height()));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_group3_segment)));
            break;
        }
        case Compression::LZW: {
            ByteBuffer decoded_bytes {};
            auto decode_lzw_segment = [&](u32 num_bytes, IntSize) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(m_stream->read_in_place<u8 const>(num_bytes));

                if (encoded_bytes.is_empty())
                    return Error::from_string_literal("TIFFImageDecoderPlugin: Unable to read from empty LZW segment");

                // Note: AFAIK, there are two common ways to use LZW compression:
                //          - With a LittleEndian stream and no Early-Change, this is used in the GIF format
                //          - With a BigEndian stream and an EarlyChange of 1, this is used in the PDF format
                //       The fun begins when they decided to change from the former to the latter when moving
                //       from TIFF 5.0 to 6.0, and without including a way for files to be identified.
                //       Fortunately, as the first byte of a LZW stream is a constant we can guess the endianess
                //       and deduce the version from it. The first code is 0x100 (9-bits).
                if (encoded_bytes[0] == 0x00)
                    decoded_bytes = TRY(Compress::LzwDecompressor<LittleEndianInputBitStream>::decompress_all(encoded_bytes, 8, 0));
                else
                    decoded_bytes = TRY(Compress::LzwDecompressor<BigEndianInputBitStream>::decompress_all(encoded_bytes, 8, -1));

                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_lzw_segment)));
            break;
        }
        case Compression::AdobeDeflate:
        case Compression::PixarDeflate: {
            // This is an extension from the Technical Notes from 2002:
            // https://web.archive.org/web/20160305055905/http://partners.adobe.com/public/developer/en/tiff/TIFFphotoshop.pdf
            ByteBuffer decoded_bytes {};
            auto decode_zlib = [&](u32 num_bytes, IntSize) -> ErrorOr<ReadonlyBytes> {
                auto stream = make<ConstrainedStream>(MaybeOwned<Stream>(*m_stream), num_bytes);
                auto decompressed_stream = TRY(Compress::ZlibDecompressor::create(move(stream)));
                decoded_bytes = TRY(decompressed_stream->read_until_eof(4096));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_zlib)));
            break;
        }
        case Compression::PackBits: {
            // Section 9: PackBits Compression
            ByteBuffer decoded_bytes {};

            auto decode_packbits_segment = [&](u32 num_bytes, IntSize) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(m_stream->read_in_place<u8 const>(num_bytes));
                decoded_bytes = TRY(Compress::PackBits::decode_all(encoded_bytes));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_packbits_segment)));
            break;
        }
        default:
            return Error::from_string_literal("This compression type is not supported yet :^)");
        }

        return {};
    }

    template<typename T>
    ErrorOr<T> read_value()
    {
        if (m_byte_order == ByteOrder::LittleEndian)
            return TRY(m_stream->read_value<LittleEndian<T>>());
        if (m_byte_order == ByteOrder::BigEndian)
            return TRY(m_stream->read_value<BigEndian<T>>());
        VERIFY_NOT_REACHED();
    }

    ErrorOr<void> set_next_ifd(u32 ifd_offset)
    {
        if (ifd_offset != 0) {
            if (ifd_offset < TRY(m_stream->tell()))
                return Error::from_string_literal("TIFFImageDecoderPlugin: Can not accept an IFD pointing to previous data");

            m_next_ifd = Optional<u32> { ifd_offset };
        } else {
            m_next_ifd = OptionalNone {};
        }
        return {};
    }

    ErrorOr<void> read_next_idf_offset()
    {
        auto const next_block_position = TRY(read_value<u32>());
        TRY(set_next_ifd(next_block_position));

        return {};
    }

    ErrorOr<void> read_image_file_header()
    {
        // Section 2: TIFF Structure - Image File Header

        auto const byte_order = TRY(m_stream->read_value<u16>());

        switch (byte_order) {
        case 0x4949:
            m_byte_order = ByteOrder::LittleEndian;
            break;
        case 0x4D4D:
            m_byte_order = ByteOrder::BigEndian;
            break;
        default:
            return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid byte order");
        }

        auto const magic_number = TRY(read_value<u16>());

        if (magic_number != 42)
            return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid magic number");

        TRY(read_next_idf_offset());

        return {};
    }

    ErrorOr<void> read_next_image_file_directory()
    {
        // Section 2: TIFF Structure - Image File Directory

        if (!m_next_ifd.has_value())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Missing an Image File Directory");

        dbgln_if(TIFF_DEBUG, "Reading image file directory at offset {}", m_next_ifd);

        TRY(m_stream->seek(m_next_ifd.value()));

        auto const number_of_field = TRY(read_value<u16>());
        auto next_tag_offset = TRY(m_stream->tell());

        for (u16 i = 0; i < number_of_field; ++i) {
            if (auto maybe_error = read_tag(); maybe_error.is_error() && TIFF_DEBUG)
                dbgln("Unable to decode tag {}/{}", i + 1, number_of_field);

            // Section 2: TIFF Structure
            // IFD Entry
            // Size of tag(u16) + type(u16) + count(u32) + value_or_offset(u32) = 12
            next_tag_offset += 12;
            TRY(m_stream->seek(next_tag_offset));
        }

        TRY(read_next_idf_offset());
        return {};
    }

    ErrorOr<Vector<Value, 1>> read_tiff_value(Type type, u32 count, u32 offset)
    {
        auto const old_offset = TRY(m_stream->tell());
        ScopeGuard reset_offset { [this, old_offset]() { MUST(m_stream->seek(old_offset)); } };

        TRY(m_stream->seek(offset));

        if (size_of_type(type) * count > m_stream->remaining())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Tag size claims to be bigger that remaining bytes");

        auto const read_every_values = [this, count]<typename T>() -> ErrorOr<Vector<Value>> {
            Vector<Value, 1> result {};
            TRY(result.try_ensure_capacity(count));
            if constexpr (IsSpecializationOf<T, Rational>) {
                for (u32 i = 0; i < count; ++i)
                    result.empend(T { TRY(read_value<typename T::Type>()), TRY(read_value<typename T::Type>()) });
            } else {
                for (u32 i = 0; i < count; ++i)
                    result.empend(typename TypePromoter<T>::Type(TRY(read_value<T>())));
            }
            return result;
        };

        switch (type) {
        case Type::Byte:
        case Type::Undefined: {
            Vector<Value, 1> result;
            auto buffer = TRY(ByteBuffer::create_uninitialized(count));
            TRY(m_stream->read_until_filled(buffer));
            result.append(move(buffer));
            return result;
        }
        case Type::ASCII:
        case Type::UTF8: {
            Vector<Value, 1> result;
            // NOTE: No need to include the null terminator
            if (count > 0)
                --count;
            auto string_data = TRY(ByteBuffer::create_uninitialized(count));
            TRY(m_stream->read_until_filled(string_data));
            result.empend(TRY(String::from_utf8(StringView { string_data.bytes() })));
            return result;
        }
        case Type::UnsignedShort:
            return read_every_values.template operator()<u16>();
        case Type::IFD:
        case Type::UnsignedLong:
            return read_every_values.template operator()<u32>();
        case Type::UnsignedRational:
            return read_every_values.template operator()<Rational<u32>>();
        case Type::SignedLong:
            return read_every_values.template operator()<i32>();
        case Type::SignedRational:
            return read_every_values.template operator()<Rational<i32>>();
        case Type::Float:
            return read_every_values.template operator()<float>();
        case Type::Double:
            return read_every_values.template operator()<double>();
        default:
            VERIFY_NOT_REACHED();
        }
    }

    ErrorOr<void> read_tag()
    {
        auto const tag = TRY(read_value<u16>());
        auto const raw_type = TRY(read_value<u16>());
        auto const type = TRY(tiff_type_from_u16(raw_type));
        auto const count = TRY(read_value<u32>());

        Checked<u32> checked_size = size_of_type(type);
        checked_size *= count;

        if (checked_size.has_overflow())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid tag with too large data");

        auto tiff_value = TRY(([=, this]() -> ErrorOr<Vector<Value>> {
            if (checked_size.value() <= 4) {
                auto value = TRY(read_tiff_value(type, count, TRY(m_stream->tell())));
                TRY(m_stream->discard(4));
                return value;
            }
            auto const offset = TRY(read_value<u32>());
            return read_tiff_value(type, count, offset);
        }()));

        auto subifd_handler = [&](u32 ifd_offset) -> ErrorOr<void> {
            if (auto result = set_next_ifd(ifd_offset); result.is_error()) {
                dbgln("{}", result.error());
                return {};
            }
            TRY(read_next_image_file_directory());
            return {};
        };

        TRY(handle_tag(move(subifd_handler), m_metadata, tag, type, count, move(tiff_value)));

        return {};
    }

    NonnullOwnPtr<FixedMemoryStream> m_stream;
    State m_state {};
    RefPtr<Bitmap> m_bitmap {};
    RefPtr<CMYKBitmap> m_cmyk_bitmap {};

    ByteOrder m_byte_order {};
    Optional<u32> m_next_ifd {};

    ExifMetadata m_metadata {};

    // These are caches for m_metadata values
    PhotometricInterpretation m_photometric_interpretation {};
    Vector<u32, 4> m_bits_per_sample {};
    u32 m_image_width {};
    Predictor m_predictor {};

    Optional<u8> m_alpha_channel_index {};
};

}

TIFFImageDecoderPlugin::TIFFImageDecoderPlugin(NonnullOwnPtr<FixedMemoryStream> stream)
{
    m_context = make<TIFF::TIFFLoadingContext>(move(stream));
}

TIFFImageDecoderPlugin::~TIFFImageDecoderPlugin() = default;

bool TIFFImageDecoderPlugin::sniff(ReadonlyBytes bytes)
{
    if (bytes.size() < 4)
        return false;
    bool const valid_little_endian = bytes[0] == 0x49 && bytes[1] == 0x49 && bytes[2] == 0x2A && bytes[3] == 0x00;
    bool const valid_big_endian = bytes[0] == 0x4D && bytes[1] == 0x4D && bytes[2] == 0x00 && bytes[3] == 0x2A;
    return valid_little_endian || valid_big_endian;
}

IntSize TIFFImageDecoderPlugin::size()
{
    return m_context->size();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> TIFFImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto stream = TRY(try_make<FixedMemoryStream>(data));
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TIFFImageDecoderPlugin(move(stream))));
    TRY(plugin->m_context->decode_image_header());
    return plugin;
}

ErrorOr<ImageFrameDescriptor> TIFFImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid frame index");

    if (m_context->state() == TIFF::TIFFLoadingContext::State::Error)
        return Error::from_string_literal("TIFFImageDecoderPlugin: Decoding failed");

    if (m_context->state() < TIFF::TIFFLoadingContext::State::FrameDecoded)
        TRY(m_context->decode_frame());

    if (m_context->cmyk_bitmap())
        return ImageFrameDescriptor { TRY(m_context->cmyk_bitmap()->to_low_quality_rgb()), 0 };

    return ImageFrameDescriptor { m_context->bitmap(), 0 };
}

Optional<Metadata const&> TIFFImageDecoderPlugin::metadata()
{
    return m_context->metadata();
}

ErrorOr<Optional<ReadonlyBytes>> TIFFImageDecoderPlugin::icc_data()
{
    return m_context->metadata().icc_profile().map([](auto const& buffer) -> ReadonlyBytes { return buffer.bytes(); });
}

ErrorOr<NonnullOwnPtr<ExifMetadata>> TIFFImageDecoderPlugin::read_exif_metadata(ReadonlyBytes data)
{
    auto stream = TRY(try_make<FixedMemoryStream>(data));
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TIFFImageDecoderPlugin(move(stream))));
    TRY(plugin->m_context->decode_image_header());
    return try_make<ExifMetadata>(plugin->m_context->metadata());
}

}
