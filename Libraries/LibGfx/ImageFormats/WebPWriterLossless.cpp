/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Lossless format: https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification

#include <AK/BitStream.h>
#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/MemoryStream.h>
#include <AK/QuickSort.h>
#include <LibCompress/Huffman.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/WebPSharedLossless.h>
#include <LibGfx/ImageFormats/WebPWriterLossless.h>

namespace Gfx {

namespace {

struct IsOpaque {
    bool is_fully_opaque { false };
    bool is_opacity_known { false };

    void set_is_fully_opaque_if_not_yet_known(bool is_fully_opaque)
    {
        if (is_opacity_known)
            return;
        this->is_fully_opaque = is_fully_opaque;
        is_opacity_known = true;
    }
};

}

NEVER_INLINE static ErrorOr<void> write_image_data(LittleEndianOutputBitStream& bit_stream, Bitmap const& bitmap, PrefixCodeGroup const& prefix_code_group)
{
    // This is currently the hot loop. Keep performance in mind when you change it.
    for (ARGB32 pixel : bitmap) {
        u8 a = pixel >> 24;
        u8 r = pixel >> 16;
        u8 g = pixel >> 8;
        u8 b = pixel;

        TRY(prefix_code_group[0].write_symbol(bit_stream, g));
        TRY(prefix_code_group[1].write_symbol(bit_stream, r));
        TRY(prefix_code_group[2].write_symbol(bit_stream, b));
        TRY(prefix_code_group[3].write_symbol(bit_stream, a));
    }
    return {};
}

struct CodeLengthSymbol {
    u8 symbol { 0 };
    u8 count { 0 }; // used for special symbols 16-18
};

// This is very similar to DeflateCompressor::encode_huffman_lengths().
// But:
// * size can be larger than 288 for green, is always 256 for r, b, a, and is always 40 for distance codes
// * code 16 has different semantics, requires last_non_zero_symbol
static size_t encode_huffman_lengths(ReadonlyBytes lengths, Array<CodeLengthSymbol, 256>& encoded_lengths)
{
    size_t encoded_count = 0;
    size_t i = 0;
    u8 last_non_zero_symbol = 8; // "If code 16 is used before a non-zero value has been emitted, a value of 8 is repeated."
    while (i < lengths.size()) {
        if (lengths[i] == 0) {
            auto zero_count = 0;
            for (size_t j = i; j < min(lengths.size(), i + 138) && lengths[j] == 0; j++)
                zero_count++;

            if (zero_count < 3) { // below minimum repeated zero count
                encoded_lengths[encoded_count++].symbol = 0;
                i++;
                continue;
            }

            if (zero_count <= 10) {
                // "Code 17 emits a streak of zeros [3..10], i.e., 3 + ReadBits(3) times."
                encoded_lengths[encoded_count].symbol = 17;
                encoded_lengths[encoded_count++].count = zero_count;
            } else {
                // "Code 18 emits a streak of zeros of length [11..138], i.e., 11 + ReadBits(7) times."
                encoded_lengths[encoded_count].symbol = 18;
                encoded_lengths[encoded_count++].count = zero_count;
            }
            i += zero_count;
            continue;
        }

        VERIFY(lengths[i] != 0);
        last_non_zero_symbol = lengths[i];
        encoded_lengths[encoded_count++].symbol = lengths[i++];

        // "Code 16 repeats the previous non-zero value [3..6] times, i.e., 3 + ReadBits(2) times."
        // This is different from deflate.
        auto copy_count = 0;
        for (size_t j = i; j < min(lengths.size(), i + 6) && lengths[j] == last_non_zero_symbol; j++)
            copy_count++;

        if (copy_count >= 3) {
            encoded_lengths[encoded_count].symbol = 16;
            encoded_lengths[encoded_count++].count = copy_count;
            i += copy_count;
            continue;
        }
    }
    return encoded_count;
}

static ErrorOr<CanonicalCode> write_simple_code_lengths(LittleEndianOutputBitStream& bit_stream, ReadonlyBytes symbols)
{
    VERIFY(symbols.size() <= 2);

    static constexpr Array<u8, 1> empty { 0 };
    if (symbols.size() == 0) {
        // "Another special case is when all prefix code lengths are zeros (an empty prefix code). [...]
        //  empty prefix codes can be coded as those containing a single symbol 0."
        symbols = empty;
    }

    unsigned non_zero_symbol_count = symbols.size();

    TRY(bit_stream.write_bits(1u, 1u));                        // Simple code length code.
    TRY(bit_stream.write_bits(non_zero_symbol_count - 1, 1u)); // num_symbols - 1
    if (symbols[0] <= 1) {
        TRY(bit_stream.write_bits(0u, 1u));         // is_first_8bits: no
        TRY(bit_stream.write_bits(symbols[0], 1u)); // symbol0
    } else {
        TRY(bit_stream.write_bits(1u, 1u));         // is_first_8bits: yes
        TRY(bit_stream.write_bits(symbols[0], 8u)); // symbol0
    }
    if (non_zero_symbol_count > 1)
        TRY(bit_stream.write_bits(symbols[1], 8u)); // symbol1

    Array<u8, 256> bits_per_symbol {};
    // "When coding a single leaf node [...], all but one code length are zeros, and the single leaf node value
    //  is marked with the length of 1 -- even when no bits are consumed when that single leaf node tree is used."
    // CanonicalCode follows that convention too, even when describing simple code lengths.
    bits_per_symbol[symbols[0]] = 1;
    if (non_zero_symbol_count > 1)
        bits_per_symbol[symbols[1]] = 1;

    return MUST(CanonicalCode::from_bytes(bits_per_symbol));
}

static ErrorOr<CanonicalCode> write_normal_code_lengths(LittleEndianOutputBitStream& bit_stream, Array<u8, 256> const& bit_lengths, size_t alphabet_size)
{
    // bit_lengths stores how many bits each symbol is encoded with.

    // Drop trailing zero lengths.
    // This will keep at least three symbols; else we would've called write_simple_code_lengths() instead.
    // This is similar to the loops in Deflate::encode_block_lengths().
    size_t code_count = bit_lengths.size();
    while (bit_lengths[code_count - 1] == 0) {
        code_count--;
        VERIFY(code_count > 2);
    }

    Array<CodeLengthSymbol, 256> encoded_lengths {};
    auto encoded_lengths_count = encode_huffman_lengths(bit_lengths.span().trim(code_count), encoded_lengths);

    // The code to compute code length code lengths is very similar to some of the code in DeflateCompressor::flush().
    // count code length frequencies
    Array<u16, 19> code_lengths_frequencies { 0 };
    for (size_t i = 0; i < encoded_lengths_count; i++) {
        VERIFY(code_lengths_frequencies[encoded_lengths[i].symbol] < UINT16_MAX);
        code_lengths_frequencies[encoded_lengths[i].symbol]++;
    }

    // generate optimal huffman code lengths code lengths
    Array<u8, 19> code_lengths_bit_lengths {};
    Compress::generate_huffman_lengths(code_lengths_bit_lengths, code_lengths_frequencies, 7); // deflate code length huffman can use up to 7 bits per symbol
    // calculate actual code length code lengths count (without trailing zeros)
    auto code_lengths_count = code_lengths_bit_lengths.size();
    while (code_lengths_bit_lengths[kCodeLengthCodeOrder[code_lengths_count - 1]] == 0)
        code_lengths_count--;

    TRY(bit_stream.write_bits(0u, 1u)); // Normal code length code.

    // This here isn't needed in Deflate because it always writes EndOfBlock. WebP does not have an EndOfBlock marker, so it needs this check.
    if (code_lengths_count < 4)
        code_lengths_count = 4;
    dbgln_if(WEBP_DEBUG, "writing code_lengths_count: {}", code_lengths_count);

    // WebP uses a different kCodeLengthCodeOrder than deflate. Other than that, the following is similar to a loop in Compress::write_dynamic_huffman().
    // "int num_code_lengths = 4 + ReadBits(4);"
    TRY(bit_stream.write_bits(code_lengths_count - 4u, 4u));

    for (size_t i = 0; i < code_lengths_count; i++) {
        TRY(bit_stream.write_bits(code_lengths_bit_lengths[kCodeLengthCodeOrder[i]], 3));
    }

    // Write code lengths. This is slightly different from deflate too -- deflate writes literal and distance lengths here,
    // while WebP writes one of these codes each for g, r, b, a, and distance.
    if (alphabet_size == encoded_lengths_count) {
        TRY(bit_stream.write_bits(0u, 1u)); // max_symbol is alphabet_size
    } else {
        dbgln_if(WEBP_DEBUG, "writing max_symbol: {}", encoded_lengths_count);
        TRY(bit_stream.write_bits(1u, 1u)); // max_symbol is explicitly coded
        // "int length_nbits = 2 + 2 * ReadBits(3);
        //  int max_symbol = 2 + ReadBits(length_nbits);"
        // => length_nbits is at most 2 + 2*7 == 16
        unsigned needed_length_nbits = encoded_lengths_count > 2 ? floor(log2(encoded_lengths_count - 2) + 1) : 2;
        VERIFY(needed_length_nbits <= 16);
        needed_length_nbits = ceil_div(needed_length_nbits, 2) * 2;
        TRY(bit_stream.write_bits((needed_length_nbits - 2) / 2, 3u));
        TRY(bit_stream.write_bits(encoded_lengths_count - 2, needed_length_nbits));
    }

    // The rest is identical to write_dynamic_huffman() again. (Code 16 has different semantics, but that doesn't matter here.)
    auto code_lengths_code = MUST(CanonicalCode::from_bytes(code_lengths_bit_lengths));
    for (size_t i = 0; i < encoded_lengths_count; i++) {
        auto encoded_length = encoded_lengths[i];
        TRY(code_lengths_code.write_symbol(bit_stream, encoded_length.symbol));
        if (encoded_length.symbol == 16) {
            // "Code 16 repeats the previous non-zero value [3..6] times, i.e., 3 + ReadBits(2) times."
            TRY(bit_stream.write_bits<u8>(encoded_length.count - 3, 2));
        } else if (encoded_length.symbol == 17) {
            // "Code 17 emits a streak of zeros [3..10], i.e., 3 + ReadBits(3) times."
            TRY(bit_stream.write_bits<u8>(encoded_length.count - 3, 3));
        } else if (encoded_length.symbol == 18) {
            // "Code 18 emits a streak of zeros of length [11..138], i.e., 11 + ReadBits(7) times."
            TRY(bit_stream.write_bits<u8>(encoded_length.count - 11, 7));
        }
    }

    return CanonicalCode::from_bytes(bit_lengths.span().trim(code_count));
}

static ErrorOr<void> write_VP8L_coded_image(ImageKind image_kind, LittleEndianOutputBitStream& bit_stream, Bitmap const& bitmap, IsOpaque& is_fully_opaque)
{
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#5_image_data
    // spatially-coded-image =  color-cache-info meta-prefix data
    // entropy-coded-image   =  color-cache-info data

    // color-cache-info      =  %b0
    // color-cache-info      =/ (%b1 4BIT) ; 1 followed by color cache size
    TRY(bit_stream.write_bits(0u, 1u)); // No color cache for now.

    if (image_kind == ImageKind::SpatiallyCoded) {
        // meta-prefix           =  %b0 / (%b1 entropy-image)
        TRY(bit_stream.write_bits(0u, 1u)); // No meta prefix for now.
    }

    // data                  =  prefix-codes lz77-coded-image
    // prefix-codes          =  prefix-code-group *prefix-codes
    // prefix-code-group     =
    //     5prefix-code ; See "Interpretation of Meta Prefix Codes" to
    //                  ; understand what each of these five prefix
    //                  ; codes are for.

    // We're writing a single prefix-code-group.
    // "These codes are (in bitstream order):

    //  Prefix code #1: Used for green channel, backward-reference length, and color cache.
    //  Prefix code #2, #3, and #4: Used for red, blue, and alpha channels, respectively.
    //  Prefix code #5: Used for backward-reference distance."

    // We use neither back-references not color cache entries yet.
    // We can make this smarter later on.

    size_t const color_cache_size = 0;
    constexpr Array alphabet_sizes = to_array<size_t>({ 256 + 24 + static_cast<size_t>(color_cache_size), 256, 256, 256, 40 });

    // If you add support for color cache: At the moment, CanonicalCodes does not support writing more than 288 symbols.
    if (alphabet_sizes[0] > 288)
        return Error::from_string_literal("Invalid alphabet size");

    // We do use huffman coding by writing a single prefix-code-group for the entire image.
    // FIXME: Consider using a meta-prefix image and using one prefix-code-group per tile.

    Array<Array<u16, 256>, 4> symbol_frequencies {};
    for (ARGB32 pixel : bitmap) {
        static constexpr auto saturating_increment = [](u16& value) {
            if (value < UINT16_MAX)
                value++;
        };
        saturating_increment(symbol_frequencies[0][(pixel >> 8) & 0xff]);  // green
        saturating_increment(symbol_frequencies[1][(pixel >> 16) & 0xff]); // red
        saturating_increment(symbol_frequencies[2][pixel & 0xff]);         // blue
        saturating_increment(symbol_frequencies[3][pixel >> 24]);          // alpha
    }

    Array<Array<u8, 256>, 4> code_lengths {};
    for (int i = 0; i < 4; ++i) {
        // "Code [0..15] indicates literal code lengths." => the maximum bit length is 15.
        Compress::generate_huffman_lengths(code_lengths[i], symbol_frequencies[i], 15);
    }

    PrefixCodeGroup prefix_code_group;
    for (int i = 0; i < 4; ++i) {
        u8 symbols[2];
        unsigned non_zero_symbol_count = 0;
        for (int j = 0; j < 256; ++j) {
            if (code_lengths[i][j] != 0) {
                if (non_zero_symbol_count < 2)
                    symbols[non_zero_symbol_count] = j;
                non_zero_symbol_count++;
            }
        }

        if (non_zero_symbol_count <= 2)
            prefix_code_group[i] = TRY(write_simple_code_lengths(bit_stream, { symbols, non_zero_symbol_count }));
        else
            prefix_code_group[i] = TRY(write_normal_code_lengths(bit_stream, code_lengths[i], alphabet_sizes[i]));

        if (i == 3)
            is_fully_opaque.set_is_fully_opaque_if_not_yet_known(non_zero_symbol_count == 1 && symbols[0] == 0xff);
    }

    // For code #5, use a simple empty code, since we don't use this yet.
    prefix_code_group[4] = TRY(write_simple_code_lengths(bit_stream, {}));

    // Image data.
    TRY(write_image_data(bit_stream, bitmap, prefix_code_group));

    return {};
}

static ARGB32 sub_argb32(ARGB32 a, ARGB32 b)
{
    auto a_color = Color::from_argb(a);
    auto b_color = Color::from_argb(b);
    return Color(a_color.red() - b_color.red(),
        a_color.green() - b_color.green(),
        a_color.blue() - b_color.blue(),
        a_color.alpha() - b_color.alpha())
        .value();
}

static ErrorOr<NonnullRefPtr<Bitmap>> maybe_write_color_indexing_transform(LittleEndianOutputBitStream& bit_stream, NonnullRefPtr<Bitmap> bitmap, IsOpaque& is_fully_opaque)
{
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#44_color_indexing_transform
    unsigned color_table_size = 0;
    HashTable<ARGB32> seen_colors;
    ARGB32 channels = 0;
    ARGB32 first_pixel = bitmap->get_pixel(0, 0).value();
    for (ARGB32 pixel : *bitmap) {
        auto result = seen_colors.set(pixel);
        if (result == HashSetResult::InsertedNewEntry) {
            ++color_table_size;
            channels |= pixel ^ first_pixel;
            if (color_table_size > 256)
                break;
        }
    }
    dbgln_if(WEBP_DEBUG, "WebP: Image has {}{} colors; all pixels or'd is {:#08x}", color_table_size > 256 ? ">= " : "", color_table_size, channels);

    // If the image has a single color, the huffman table can encode it in 0 bits and color indexing does not help.
    if (color_table_size <= 1 || color_table_size > 256)
        return bitmap;

    // If all colors use just a single channel, color indexing does not help either,
    // except if there are <= 16 colors and we can do pixel bundling.
    // FIXME: Once we support color cache, maybe that helps for single-channel pixels with fewer than 16 colors
    //        and we don't need to write a color index then?
    if (color_table_size > 16) {
        int number_of_non_constant_channels = 0;
        for (int i = 0; i < 4; ++i) {
            if (channels & (0xff << (i * 8)))
                number_of_non_constant_channels++;
        }
        if (number_of_non_constant_channels <= 1)
            return bitmap;
    }

    dbgln_if(WEBP_DEBUG, "WebP: Writing color index transform");
    TRY(bit_stream.write_bits(1u, 1u)); // Transform present.
    TRY(bit_stream.write_bits(static_cast<unsigned>(COLOR_INDEXING_TRANSFORM), 2u));

    // "int color_table_size = ReadBits(8) + 1;"
    TRY(bit_stream.write_bits(color_table_size - 1, 8u));

    // Store color index to bit stream.
    Vector<ARGB32, 256> colors;
    for (ARGB32 color : seen_colors)
        colors.append(color);
    quick_sort(colors.begin(), colors.end());

    // "The color table is stored using the image storage format itself." [...]
    // "The color table is always subtraction-coded to reduce image entropy."
    auto color_index_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, { static_cast<int>(color_table_size), 1 }));
    color_index_bitmap->set_pixel(0, 0, Color::from_argb(colors[0]));
    for (unsigned i = 1; i < color_table_size; ++i)
        color_index_bitmap->set_pixel(i, 0, Color::from_argb(sub_argb32(colors[i], colors[i - 1])));
    TRY(write_VP8L_coded_image(ImageKind::EntropyCoded, bit_stream, *color_index_bitmap, is_fully_opaque));

    // Return a new bitmap with the color indexing transform applied.
    HashMap<ARGB32, u8> color_index_map;
    for (unsigned i = 0; i < color_table_size; ++i)
        color_index_map.set(colors[i], i);

    // "When the color table is small (equal to or less than 16 colors), several pixels are bundled into a single pixel.
    //  The pixel bundling packs several (2, 4, or 8) pixels into a single pixel, reducing the image width respectively."
    int width_bits;
    if (color_table_size <= 2)
        width_bits = 3;
    else if (color_table_size <= 4)
        width_bits = 2;
    else if (color_table_size <= 16)
        width_bits = 1;
    else
        width_bits = 0;
    int pixels_per_pixel = 1 << width_bits;
    int image_width = ceil_div(bitmap->width(), pixels_per_pixel);
    auto new_bitmap = TRY(Bitmap::create(BitmapFormat::BGRx8888, { image_width, bitmap->height() }));

    unsigned bits_per_pixel = 8 / pixels_per_pixel;
    for (int y = 0; y < bitmap->height(); ++y) {
        for (int x = 0, new_x = 0; x < bitmap->width(); x += pixels_per_pixel, ++new_x) {
            u8 indexes = 0;
            for (int i = 0; i < pixels_per_pixel && x + i < bitmap->width(); ++i) {
                auto pixel = bitmap->get_pixel(x + i, y);
                auto result = color_index_map.get(pixel.value());
                VERIFY(result.has_value());
                indexes |= result.value() << (i * bits_per_pixel);
            }
            new_bitmap->set_pixel(new_x, y, Color(0, indexes, 0, 0));
        }
    }

    return new_bitmap;
}

static ErrorOr<void> write_VP8L_image_data(Stream& stream, NonnullRefPtr<Bitmap> bitmap, VP8LEncoderOptions const& options, IsOpaque& is_fully_opaque)
{
    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // image-stream  = optional-transform spatially-coded-image
    // optional-transform   =  (%b1 transform optional-transform) / %b0
    if (options.allowed_transforms & (1u << COLOR_INDEXING_TRANSFORM))
        bitmap = TRY(maybe_write_color_indexing_transform(bit_stream, bitmap, is_fully_opaque));
    TRY(bit_stream.write_bits(0u, 1u)); // No further transforms for now.

    TRY(write_VP8L_coded_image(ImageKind::SpatiallyCoded, bit_stream, *bitmap, is_fully_opaque));

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.align_to_byte_boundary());
    TRY(bit_stream.flush_buffer_to_stream());

    return {};
}

ErrorOr<ByteBuffer> compress_VP8L_image_data(Bitmap const& bitmap, VP8LEncoderOptions const& options, bool& is_fully_opaque)
{
    AllocatingMemoryStream vp8l_data_stream;
    IsOpaque is_opaque_struct;
    TRY(write_VP8L_image_data(vp8l_data_stream, bitmap, options, is_opaque_struct));
    VERIFY(is_opaque_struct.is_opacity_known);
    is_fully_opaque = is_opaque_struct.is_fully_opaque;
    return vp8l_data_stream.read_until_eof();
}

}
