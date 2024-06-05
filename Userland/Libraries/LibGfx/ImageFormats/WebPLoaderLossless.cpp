/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitStream.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/MemoryStream.h>
#include <AK/Vector.h>
#include <LibCompress/Deflate.h>
#include <LibGfx/ImageFormats/WebPLoaderLossless.h>
#include <LibGfx/ImageFormats/WebPSharedLossless.h>

// Lossless format: https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification

namespace Gfx {

// https://developers.google.com/speed/webp/docs/riff_container#simple_file_format_lossless
// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#7_overall_structure_of_the_format
ErrorOr<VP8LHeader> decode_webp_chunk_VP8L_header(ReadonlyBytes vp8l_data)
{
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#3_riff_header
    if (vp8l_data.size() < 5)
        return Error::from_string_literal("WebPImageDecoderPlugin: VP8L chunk too small");

    FixedMemoryStream memory_stream { vp8l_data.trim(5) };
    LittleEndianInputBitStream bit_stream { MaybeOwned<Stream>(memory_stream), LittleEndianInputBitStream::UnsatisfiableReadBehavior::FillWithZero };

    u8 signature = TRY(bit_stream.read_bits(8));
    if (signature != 0x2f)
        return Error::from_string_literal("WebPImageDecoderPlugin: VP8L chunk invalid signature");

    // 14 bits width-1, 14 bits height-1, 1 bit alpha hint, 3 bit version_number.
    u16 width = TRY(bit_stream.read_bits(14)) + 1;
    u16 height = TRY(bit_stream.read_bits(14)) + 1;
    bool is_alpha_used = TRY(bit_stream.read_bits(1)) != 0;
    u8 version_number = TRY(bit_stream.read_bits(3));
    VERIFY(bit_stream.is_eof());

    dbgln_if(WEBP_DEBUG, "VP8L: width {}, height {}, is_alpha_used {}, version_number {}",
        width, height, is_alpha_used, version_number);

    // "The version_number is a 3 bit code that must be set to 0. Any other value should be treated as an error."
    if (version_number != 0)
        return Error::from_string_literal("WebPImageDecoderPlugin: VP8L chunk invalid version_number");

    return VP8LHeader { width, height, is_alpha_used, vp8l_data.slice(5) };
}

// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#621_decoding_and_building_the_prefix_codes
static ErrorOr<CanonicalCode> decode_webp_chunk_VP8L_prefix_code(LittleEndianInputBitStream& bit_stream, size_t alphabet_size)
{
    // prefix-code           =  simple-prefix-code / normal-prefix-code
    bool is_simple_code_length_code = TRY(bit_stream.read_bits(1));
    dbgln_if(WEBP_DEBUG, "is_simple_code_length_code {}", is_simple_code_length_code);

    Vector<u8, 286> code_lengths;

    if (is_simple_code_length_code) {
        TRY(code_lengths.try_resize(alphabet_size));

        int num_symbols = TRY(bit_stream.read_bits(1)) + 1;
        int is_first_8bits = TRY(bit_stream.read_bits(1));
        u8 symbol0 = TRY(bit_stream.read_bits(1 + 7 * is_first_8bits));
        dbgln_if(WEBP_DEBUG, "  symbol0 {}", symbol0);

        if (symbol0 >= code_lengths.size())
            return Error::from_string_literal("symbol0 out of bounds");
        code_lengths[symbol0] = 1;
        if (num_symbols == 2) {
            u8 symbol1 = TRY(bit_stream.read_bits(8));
            dbgln_if(WEBP_DEBUG, "  symbol1 {}", symbol1);

            if (symbol1 >= code_lengths.size())
                return Error::from_string_literal("symbol1 out of bounds");
            code_lengths[symbol1] = 1;
        }

        return CanonicalCode::from_bytes(code_lengths);
    }

    // This has plenty in common with deflate (cf DeflateDecompressor::decode_codes() in Deflate.cpp in LibCompress)
    // Symbol 16 has different semantics, and kCodeLengthCodeOrder is different. Other than that, this is virtually deflate.
    // (...but webp uses 5 different prefix codes, while deflate doesn't.)
    int num_code_lengths = 4 + TRY(bit_stream.read_bits(4));
    dbgln_if(WEBP_DEBUG, "  num_code_lengths {}", num_code_lengths);
    VERIFY(num_code_lengths <= 19);

    u8 code_length_code_lengths[kCodeLengthCodeOrder.size()] = { 0 }; // "All zeros" [sic]
    for (int i = 0; i < num_code_lengths; ++i)
        code_length_code_lengths[kCodeLengthCodeOrder[i]] = TRY(bit_stream.read_bits(3));

    // "Next, if `ReadBits(1) == 0`, the maximum number of different read symbols
    //  (`max_symbol`) for each symbol type (A, R, G, B, and distance) is set to its
    //  alphabet size:"
    unsigned max_symbol;
    if (TRY(bit_stream.read_bits(1)) == 0) {
        max_symbol = alphabet_size;
    }
    // "Otherwise, it is defined as:"
    else {
        // "int length_nbits = 2 + 2 * ReadBits(3);"
        int length_nbits = 2 + 2 * TRY(bit_stream.read_bits(3));
        // "int max_symbol = 2 + ReadBits(length_nbits);"
        max_symbol = 2 + TRY(bit_stream.read_bits(length_nbits));
        dbgln_if(WEBP_DEBUG, "  extended, length_nbits {} max_symbol {}", length_nbits, max_symbol);

        // "If `max_symbol` is larger than the size of the alphabet for the symbol type, the bitstream is invalid."
        if (max_symbol > alphabet_size)
            return Error::from_string_literal("WebPImageDecoderPlugin: invalid max_symbol");
    }

    // "A prefix table is then built from code_length_code_lengths and used to read up to max_symbol code lengths."
    dbgln_if(WEBP_DEBUG, "  reading {} symbols from at most {} codes", alphabet_size, max_symbol);
    auto const code_length_code = TRY(CanonicalCode::from_bytes({ code_length_code_lengths, sizeof(code_length_code_lengths) }));
    u8 last_non_zero = 8; // "If code 16 is used before a non-zero value has been emitted, a value of 8 is repeated."
    while (code_lengths.size() < alphabet_size) {
        if (max_symbol == 0)
            break;
        --max_symbol;

        auto symbol = TRY(code_length_code.read_symbol(bit_stream));

        if (symbol < 16) {
            // "Code [0..15] indicates literal code lengths."
            code_lengths.append(static_cast<u8>(symbol));
            if (symbol != 0)
                last_non_zero = symbol;
        } else if (symbol == 16) {
            // "Code 16 repeats the previous non-zero value [3..6] times, i.e., 3 + ReadBits(2) times."
            auto nrepeat = 3 + TRY(bit_stream.read_bits(2));

            // This is different from deflate.
            for (size_t j = 0; j < nrepeat; ++j)
                code_lengths.append(last_non_zero);
        } else if (symbol == 17) {
            // "Code 17 emits a streak of zeros [3..10], i.e., 3 + ReadBits(3) times."
            auto nrepeat = 3 + TRY(bit_stream.read_bits(3));
            for (size_t j = 0; j < nrepeat; ++j)
                code_lengths.append(0);
        } else {
            VERIFY(symbol == 18);
            // "Code 18 emits a streak of zeros of length [11..138], i.e., 11 + ReadBits(7) times."
            auto nrepeat = 11 + TRY(bit_stream.read_bits(7));
            for (size_t j = 0; j < nrepeat; ++j)
                code_lengths.append(0);
        }
    }

    if (code_lengths.size() > alphabet_size)
        return Error::from_string_literal("Number of code lengths is larger than the alphabet size");

    return CanonicalCode::from_bytes(code_lengths);
}

// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#622_decoding_of_meta_prefix_codes
// The description of prefix code groups is in "Decoding of Meta Prefix Codes", even though prefix code groups are used
// in regular images without meta prefix code as well ¯\_(ツ)_/¯.
static ErrorOr<PrefixCodeGroup> decode_webp_chunk_VP8L_prefix_code_group(u16 color_cache_size, LittleEndianInputBitStream& bit_stream)
{
    // prefix-code-group     =
    //     5prefix-code ; See "Interpretation of Meta Prefix Codes" to
    //                  ; understand what each of these five prefix
    //                  ; codes are for.

    // "Once code lengths are read, a prefix code for each symbol type (A, R, G, B, distance) is formed using their respective alphabet sizes."
    // ...
    // "* G channel: 256 + 24 + color_cache_size
    //  * other literals (A,R,B): 256
    //  * distance code: 40"
    Array<size_t, 5> const alphabet_sizes { 256 + 24 + static_cast<size_t>(color_cache_size), 256, 256, 256, 40 };

    PrefixCodeGroup group;
    for (size_t i = 0; i < alphabet_sizes.size(); ++i)
        group[i] = TRY(decode_webp_chunk_VP8L_prefix_code(bit_stream, alphabet_sizes[i]));
    return group;
}

static ErrorOr<NonnullRefPtr<Bitmap>> decode_webp_chunk_VP8L_image(ImageKind image_kind, BitmapFormat format, IntSize const& size, LittleEndianInputBitStream& bit_stream)
{
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#623_decoding_entropy-coded_image_data
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#523_color_cache_coding
    // spatially-coded-image =  color-cache-info meta-prefix data
    // entropy-coded-image   =  color-cache-info data

    // color-cache-info      =  %b0
    // color-cache-info      =/ (%b1 4BIT) ; 1 followed by color cache size
    bool has_color_cache_info = TRY(bit_stream.read_bits(1));
    u16 color_cache_size = 0;
    u8 color_cache_code_bits;
    dbgln_if(WEBP_DEBUG, "has_color_cache_info {}", has_color_cache_info);
    Vector<ARGB32, 32> color_cache;
    if (has_color_cache_info) {
        color_cache_code_bits = TRY(bit_stream.read_bits(4));

        // "The range of allowed values for color_cache_code_bits is [1..11]. Compliant decoders must indicate a corrupted bitstream for other values."
        if (color_cache_code_bits < 1 || color_cache_code_bits > 11)
            return Error::from_string_literal("WebPImageDecoderPlugin: VP8L invalid color_cache_code_bits");

        color_cache_size = 1 << color_cache_code_bits;
        dbgln_if(WEBP_DEBUG, "color_cache_size {}", color_cache_size);

        TRY(color_cache.try_resize(color_cache_size));
    }

    int num_prefix_groups = 1;
    RefPtr<Gfx::Bitmap> entropy_image;
    int prefix_bits = 0;
    if (image_kind == ImageKind::SpatiallyCoded) {
        // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#622_decoding_of_meta_prefix_codes
        // In particular, the "Entropy image" subsection.
        // "Meta prefix codes may be used only when the image is being used in the role of an ARGB image."
        // meta-prefix           =  %b0 / (%b1 entropy-image)
        bool has_meta_prefix = TRY(bit_stream.read_bits(1));
        dbgln_if(WEBP_DEBUG, "has_meta_prefix {}", has_meta_prefix);
        if (has_meta_prefix) {
            prefix_bits = TRY(bit_stream.read_bits(3)) + 2;
            dbgln_if(WEBP_DEBUG, "prefix_bits {}", prefix_bits);
            int block_size = 1 << prefix_bits;
            IntSize prefix_size { ceil_div(size.width(), block_size), ceil_div(size.height(), block_size) };

            entropy_image = TRY(decode_webp_chunk_VP8L_image(ImageKind::EntropyCoded, BitmapFormat::BGRx8888, prefix_size, bit_stream));

            // A "meta prefix image" or "entropy image" can tell the decoder to use different PrefixCodeGroup for
            // tiles of the main, spatially coded, image. It's a bit hidden in the spec:
            //      "The red and green components of a pixel define the meta prefix code used in a particular block of the ARGB image."
            //      ...
            //      "The number of prefix code groups in the ARGB image can be obtained by finding the largest meta prefix code from the entropy image"
            // That is, if a meta prefix image is present, the main image has more than one PrefixCodeGroup,
            // and the highest value in the meta prefix image determines how many exactly.
            u16 largest_meta_prefix_code = 0;
            for (ARGB32& pixel : *entropy_image) {
                u16 meta_prefix_code = (pixel >> 8) & 0xffff;
                if (meta_prefix_code > largest_meta_prefix_code)
                    largest_meta_prefix_code = meta_prefix_code;
            }
            dbgln_if(WEBP_DEBUG, "largest meta prefix code {}", largest_meta_prefix_code);

            num_prefix_groups = largest_meta_prefix_code + 1;
        }
    }

    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#52_encoding_of_image_data
    // "The encoded image data consists of several parts:
    //    1. Decoding and building the prefix codes
    //    2. Meta prefix codes
    //    3. Entropy-coded image data"
    // data                  =  prefix-codes lz77-coded-image
    // prefix-codes          =  prefix-code-group *prefix-codes

    Vector<PrefixCodeGroup, 1> groups;
    for (int i = 0; i < num_prefix_groups; ++i)
        TRY(groups.try_append(TRY(decode_webp_chunk_VP8L_prefix_code_group(color_cache_size, bit_stream))));

    auto bitmap = TRY(Bitmap::create(format, size));

    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#522_lz77_backward_reference
    struct Offset {
        i8 x, y;
    };
    // clang-format off
    Array<Offset, 120> distance_map { {
        {0, 1}, {1, 0},
        {1, 1}, {-1, 1}, {0, 2}, { 2, 0},
        {1, 2}, {-1, 2}, {2, 1}, {-2, 1},
        {2, 2}, {-2, 2}, {0, 3}, { 3, 0}, { 1, 3}, {-1, 3}, { 3, 1}, {-3, 1},
        {2, 3}, {-2, 3}, {3, 2}, {-3, 2}, { 0, 4}, { 4, 0}, { 1, 4}, {-1, 4}, { 4, 1}, {-4, 1},
        {3, 3}, {-3, 3}, {2, 4}, {-2, 4}, { 4, 2}, {-4, 2}, { 0, 5},
        {3, 4}, {-3, 4}, {4, 3}, {-4, 3}, { 5, 0}, { 1, 5}, {-1, 5}, { 5, 1}, {-5, 1}, { 2, 5}, {-2, 5}, { 5, 2}, {-5, 2},
        {4, 4}, {-4, 4}, {3, 5}, {-3, 5}, { 5, 3}, {-5, 3}, { 0, 6}, { 6, 0}, { 1, 6}, {-1, 6}, { 6, 1}, {-6, 1}, { 2, 6}, {-2, 6}, {6, 2}, {-6, 2},
        {4, 5}, {-4, 5}, {5, 4}, {-5, 4}, { 3, 6}, {-3, 6}, { 6, 3}, {-6, 3}, { 0, 7}, { 7, 0}, { 1, 7}, {-1, 7},
        {5, 5}, {-5, 5}, {7, 1}, {-7, 1}, { 4, 6}, {-4, 6}, { 6, 4}, {-6, 4}, { 2, 7}, {-2, 7}, { 7, 2}, {-7, 2}, { 3, 7}, {-3, 7}, {7, 3}, {-7, 3},
        {5, 6}, {-5, 6}, {6, 5}, {-6, 5}, { 8, 0}, { 4, 7}, {-4, 7}, { 7, 4}, {-7, 4}, { 8, 1}, { 8, 2},
        {6, 6}, {-6, 6}, {8, 3}, { 5, 7}, {-5, 7}, { 7, 5}, {-7, 5}, { 8, 4},
        {6, 7}, {-6, 7}, {7, 6}, {-7, 6}, { 8, 5},
        {7, 7}, {-7, 7}, {8, 6},
        {8, 7},
    } };
    // clang-format on

    // lz77-coded-image      =
    //     *((argb-pixel / lz77-copy / color-cache-code) lz77-coded-image)
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#623_decoding_entropy-coded_image_data
    ARGB32* begin = bitmap->begin();
    ARGB32* end = bitmap->end();
    ARGB32* pixel = begin;

    auto prefix_group = [prefix_bits, begin, &groups, size, &entropy_image](ARGB32* pixel) -> PrefixCodeGroup const& {
        if (!prefix_bits)
            return groups[0];

        size_t offset = pixel - begin;
        int x = offset % size.width();
        int y = offset / size.width();

        int meta_prefix_code = (entropy_image->scanline(y >> prefix_bits)[x >> prefix_bits] >> 8) & 0xffff;
        return groups[meta_prefix_code];
    };

    auto emit_pixel = [&pixel, &color_cache, color_cache_size, color_cache_code_bits](ARGB32 color) {
        // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#523_color_cache_coding
        // "The state of the color cache is maintained by inserting every pixel, be it produced by backward referencing or as literals, into the cache in the order they appear in the stream."
        *pixel++ = color;
        if (color_cache_size)
            color_cache[(0x1e35a7bd * color) >> (32 - color_cache_code_bits)] = color;
    };

    while (pixel < end) {
        // "For the current position (x, y) in the image, the decoder first identifies the corresponding prefix code group"
        auto const& group = prefix_group(pixel);

        // "Next, read the symbol S from the bitstream using prefix code #1.
        //  Note that S is any integer in the range 0 to (256 + 24 + color_cache_size - 1)."
        auto symbol = TRY(group[0].read_symbol(bit_stream));
        if (symbol >= 256u + 24u + color_cache_size)
            return Error::from_string_literal("WebPImageDecoderPlugin: Symbol out of bounds");

        // "1. if S < 256"
        if (symbol < 256u) {
            // "a. Use S as the green component."
            u8 g = symbol;

            // "b. Read red from the bitstream using prefix code #2."
            u8 r = TRY(group[1].read_symbol(bit_stream));

            // "c. Read blue from the bitstream using prefix code #3."
            u8 b = TRY(group[2].read_symbol(bit_stream));

            // "d. Read alpha from the bitstream using prefix code #4."
            u8 a = TRY(group[3].read_symbol(bit_stream));

            emit_pixel(Color(r, g, b, a).value());
        }
        // "2. if S >= 256 && S < 256 + 24"
        else if (symbol < 256u + 24u) {
            auto prefix_value = [&bit_stream](u8 prefix_code) -> ErrorOr<u32> {
                // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#522_lz77_backward_reference
                if (prefix_code < 4)
                    return prefix_code + 1;
                int extra_bits = (prefix_code - 2) >> 1;
                int offset = (2 + (prefix_code & 1)) << extra_bits;
                return offset + TRY(bit_stream.read_bits(extra_bits)) + 1;
            };

            // "a. Use S - 256 as a length prefix code."
            u8 length_prefix_code = symbol - 256;

            // "b. Read extra bits for length from the bitstream."
            // "c. Determine backward-reference length L from length prefix code and the extra bits read."
            u32 length = TRY(prefix_value(length_prefix_code));

            // "d. Read distance prefix code from the bitstream using prefix code #5."
            u8 distance_prefix_code = TRY(group[4].read_symbol(bit_stream));

            // "e. Read extra bits for distance from the bitstream."
            // "f. Determine backward-reference distance D from distance prefix code and the extra bits read."
            i32 distance = TRY(prefix_value(distance_prefix_code));

            // "g. Copy the L pixels (in scan-line order) from the sequence of pixels prior to them by D pixels."

            // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#522_lz77_backward_reference
            // "Distance codes larger than 120 denote the pixel-distance in scan-line order, offset by 120."
            // "The smallest distance codes [1..120] are special, and are reserved for a close neighborhood of the current pixel."
            if (distance <= 120) {
                auto offset = distance_map[distance - 1];
                distance = offset.x + offset.y * bitmap->width();
                if (distance < 1)
                    distance = 1;
            } else {
                distance = distance - 120;
            }

            if (pixel - begin < distance) {
                dbgln_if(WEBP_DEBUG, "invalid backref, {} < {}", pixel - begin, distance);
                return Error::from_string_literal("WebPImageDecoderPlugin: Backward reference distance out of bounds");
            }

            if (end - pixel < static_cast<ptrdiff_t>(length)) {
                dbgln_if(WEBP_DEBUG, "invalid length, {} < {}", end - pixel, length);
                return Error::from_string_literal("WebPImageDecoderPlugin: Backward reference length out of bounds");
            }

            ARGB32* src = pixel - distance;
            for (u32 i = 0; i < length; ++i)
                emit_pixel(src[i]);
        }
        // "3. if S >= 256 + 24"
        else {
            // "a. Use S - (256 + 24) as the index into the color cache."
            unsigned index = symbol - (256 + 24);

            // "b. Get ARGB color from the color cache at that index."
            // `symbol` is bounds-checked at the start of the loop.
            *pixel++ = color_cache[index];
        }
    }

    return bitmap;
}

namespace {

static ARGB32 add_argb32(ARGB32 a, ARGB32 b)
{
    auto a_color = Color::from_argb(a);
    auto b_color = Color::from_argb(b);
    return Color(a_color.red() + b_color.red(),
        a_color.green() + b_color.green(),
        a_color.blue() + b_color.blue(),
        a_color.alpha() + b_color.alpha())
        .value();
}

class Transform {
public:
    virtual ~Transform();

    // Could modify the input bitmap and return it, or could return a new bitmap.
    virtual ErrorOr<NonnullRefPtr<Bitmap>> transform(NonnullRefPtr<Bitmap>) = 0;
};

Transform::~Transform() = default;

// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#41_predictor_transform
class PredictorTransform : public Transform {
public:
    static ErrorOr<NonnullOwnPtr<PredictorTransform>> read(LittleEndianInputBitStream&, IntSize const& image_size);
    virtual ErrorOr<NonnullRefPtr<Bitmap>> transform(NonnullRefPtr<Bitmap>) override;

private:
    PredictorTransform(int size_bits, NonnullRefPtr<Bitmap> predictor_bitmap)
        : m_size_bits(size_bits)
        , m_predictor_bitmap(move(predictor_bitmap))
    {
    }

    // These capitalized functions are all from the spec:
    static u8 Average2(u8 a, u8 b)
    {
        return (a + b) / 2;
    }

    static u32 Select(u32 L, u32 T, u32 TL)
    {
        // "L = left pixel, T = top pixel, TL = top left pixel."

#define ALPHA(x) ((x >> 24) & 0xff)
#define RED(x) ((x >> 16) & 0xff)
#define GREEN(x) ((x >> 8) & 0xff)
#define BLUE(x) (x & 0xff)

        // "ARGB component estimates for prediction."
        int pAlpha = ALPHA(L) + ALPHA(T) - ALPHA(TL);
        int pRed = RED(L) + RED(T) - RED(TL);
        int pGreen = GREEN(L) + GREEN(T) - GREEN(TL);
        int pBlue = BLUE(L) + BLUE(T) - BLUE(TL);

        // "Manhattan distances to estimates for left and top pixels."
        int pL = abs(pAlpha - (int)ALPHA(L)) + abs(pRed - (int)RED(L)) + abs(pGreen - (int)GREEN(L)) + abs(pBlue - (int)BLUE(L));
        int pT = abs(pAlpha - (int)ALPHA(T)) + abs(pRed - (int)RED(T)) + abs(pGreen - (int)GREEN(T)) + abs(pBlue - (int)BLUE(T));

        // "Return either left or top, the one closer to the prediction."
        if (pL < pT) {
            return L;
        } else {
            return T;
        }

#undef BLUE
#undef GREEN
#undef RED
#undef ALPHA
    }

    // "Clamp the input value between 0 and 255."
    static int Clamp(int a)
    {
        return clamp(a, 0, 255);
    }

    static int ClampAddSubtractFull(int a, int b, int c)
    {
        return Clamp(a + b - c);
    }

    static int ClampAddSubtractHalf(int a, int b)
    {
        return Clamp(a + (a - b) / 2);
    }

    // ...and we're back from the spec!
    static Color average2(Color a, Color b)
    {
        return Color(Average2(a.red(), b.red()),
            Average2(a.green(), b.green()),
            Average2(a.blue(), b.blue()),
            Average2(a.alpha(), b.alpha()));
    }

    static ARGB32 average2(ARGB32 a, ARGB32 b)
    {
        return average2(Color::from_argb(a), Color::from_argb(b)).value();
    }

    static ErrorOr<ARGB32> predict(u8 predictor, ARGB32 TL, ARGB32 T, ARGB32 TR, ARGB32 L);

    int m_size_bits;
    NonnullRefPtr<Bitmap> m_predictor_bitmap;
};

ErrorOr<NonnullOwnPtr<PredictorTransform>> PredictorTransform::read(LittleEndianInputBitStream& bit_stream, IntSize const& image_size)
{
    // predictor-image      =  3BIT ; sub-pixel code
    //                         entropy-coded-image
    int size_bits = TRY(bit_stream.read_bits(3)) + 2;
    dbgln_if(WEBP_DEBUG, "predictor size_bits {}", size_bits);

    int block_size = 1 << size_bits;
    IntSize predictor_image_size { ceil_div(image_size.width(), block_size), ceil_div(image_size.height(), block_size) };

    auto predictor_bitmap = TRY(decode_webp_chunk_VP8L_image(ImageKind::EntropyCoded, BitmapFormat::BGRx8888, predictor_image_size, bit_stream));

    return adopt_nonnull_own_or_enomem(new (nothrow) PredictorTransform(size_bits, move(predictor_bitmap)));
}

ErrorOr<NonnullRefPtr<Bitmap>> PredictorTransform::transform(NonnullRefPtr<Bitmap> bitmap_ref)
{
    Bitmap& bitmap = *bitmap_ref;

    // "There are special handling rules for some border pixels.
    //  If there is a prediction transform, regardless of the mode [0..13] for these pixels,
    //  the predicted value for the left-topmost pixel of the image is 0xff000000,
    bitmap.scanline(0)[0] = add_argb32(bitmap.scanline(0)[0], 0xff000000);

    //  L-pixel for all pixels on the top row,
    for (int x = 1; x < bitmap.width(); ++x)
        bitmap.scanline(0)[x] = add_argb32(bitmap.scanline(0)[x], bitmap.scanline(0)[x - 1]);

    //  and T-pixel for all pixels on the leftmost column."
    for (int y = 1; y < bitmap.height(); ++y)
        bitmap.scanline(y)[0] = add_argb32(bitmap.scanline(y)[0], bitmap.scanline(y - 1)[0]);

    ARGB32* bitmap_previous_scanline = bitmap.scanline(0);
    for (int y = 1; y < bitmap.height(); ++y) {
        ARGB32* bitmap_scanline = bitmap.scanline(y);

        ARGB32 TL = bitmap_previous_scanline[0];
        ARGB32 T = bitmap_previous_scanline[1];
        ARGB32 TR = 2 < bitmap.width() ? bitmap_previous_scanline[2] : bitmap_previous_scanline[0];

        ARGB32 L = bitmap_scanline[0];

        int predictor_y = y >> m_size_bits;
        ARGB32* predictor_scanline = m_predictor_bitmap->scanline(predictor_y);

        for (int x = 1; x < bitmap.width(); ++x) {
            int predictor_x = x >> m_size_bits;

            // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#51_roles_of_image_data
            // "The green component of a pixel defines which of the 14 predictors is used within a particular block of the ARGB image."
            u8 predictor = Color::from_argb(predictor_scanline[predictor_x]).green();

            ARGB32 predicted = TRY(predict(predictor, TL, T, TR, L));

            // "The final pixel value is obtained by adding each channel of the predicted value to the encoded residual value."
            bitmap_scanline[x] = add_argb32(bitmap_scanline[x], predicted);

            TL = T;
            T = TR;

            // "Addressing the TR-pixel for pixels on the rightmost column is exceptional.
            //  The pixels on the rightmost column are predicted by using the modes [0..13] just like pixels not on the border,
            //  but the leftmost pixel on the same row as the current pixel is instead used as the TR-pixel."
            TR = x + 2 < bitmap.width() ? bitmap_previous_scanline[x + 2] : bitmap_previous_scanline[0];

            L = bitmap_scanline[x];
        }

        bitmap_previous_scanline = bitmap_scanline;
    }
    return bitmap_ref;
}

ErrorOr<ARGB32> PredictorTransform::predict(u8 predictor, ARGB32 TL, ARGB32 T, ARGB32 TR, ARGB32 L)
{
    switch (predictor) {
    case 0:
        // "0xff000000 (represents solid black color in ARGB)"
        return 0xff000000;
    case 1:
        // "L"
        return L;
    case 2:
        // "T"
        return T;
    case 3:
        // "TR"
        return TR;
    case 4:
        // "TL"
        return TL;
    case 5:
        // "Average2(Average2(L, TR), T)"
        return average2(average2(L, TR), T);
    case 6:
        // "Average2(L, TL)"
        return average2(L, TL);
    case 7:
        // "Average2(L, T)"
        return average2(L, T);
    case 8:
        // "Average2(TL, T)"
        return average2(TL, T);
    case 9:
        // "Average2(T, TR)"
        return average2(T, TR);
    case 10:
        // "Average2(Average2(L, TL), Average2(T, TR))"
        return average2(average2(L, TL), average2(T, TR));
    case 11:
        // "Select(L, T, TL)"
        return Select(L, T, TL);
    case 12: {
        // "ClampAddSubtractFull(L, T, TL)"
        auto color_L = Color::from_argb(L);
        auto color_T = Color::from_argb(T);
        auto color_TL = Color::from_argb(TL);
        return Color(ClampAddSubtractFull(color_L.red(), color_T.red(), color_TL.red()),
            ClampAddSubtractFull(color_L.green(), color_T.green(), color_TL.green()),
            ClampAddSubtractFull(color_L.blue(), color_T.blue(), color_TL.blue()),
            ClampAddSubtractFull(color_L.alpha(), color_T.alpha(), color_TL.alpha()))
            .value();
    }
    case 13: {
        // "ClampAddSubtractHalf(Average2(L, T), TL)"
        auto color_L = Color::from_argb(L);
        auto color_T = Color::from_argb(T);
        auto color_TL = Color::from_argb(TL);
        return Color(ClampAddSubtractHalf(Average2(color_L.red(), color_T.red()), color_TL.red()),
            ClampAddSubtractHalf(Average2(color_L.green(), color_T.green()), color_TL.green()),
            ClampAddSubtractHalf(Average2(color_L.blue(), color_T.blue()), color_TL.blue()),
            ClampAddSubtractHalf(Average2(color_L.alpha(), color_T.alpha()), color_TL.alpha()))
            .value();
    }
    }
    return Error::from_string_literal("WebPImageDecoderPlugin: invalid predictor");
}

// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#42_color_transform
class ColorTransform : public Transform {
public:
    static ErrorOr<NonnullOwnPtr<ColorTransform>> read(LittleEndianInputBitStream&, IntSize const& image_size);
    virtual ErrorOr<NonnullRefPtr<Bitmap>> transform(NonnullRefPtr<Bitmap>) override;

private:
    ColorTransform(int size_bits, NonnullRefPtr<Bitmap> color_bitmap)
        : m_size_bits(size_bits)
        , m_color_bitmap(move(color_bitmap))
    {
    }

    static i8 ColorTransformDelta(i8 transform, i8 color)
    {
        return (transform * color) >> 5;
    }

    static ARGB32 inverse_transform(ARGB32 pixel, ARGB32 transform);

    int m_size_bits;
    NonnullRefPtr<Bitmap> m_color_bitmap;
};

ErrorOr<NonnullOwnPtr<ColorTransform>> ColorTransform::read(LittleEndianInputBitStream& bit_stream, IntSize const& image_size)
{
    // color-image          =  3BIT ; sub-pixel code
    //                         entropy-coded-image
    int size_bits = TRY(bit_stream.read_bits(3)) + 2;
    dbgln_if(WEBP_DEBUG, "color size_bits {}", size_bits);

    int block_size = 1 << size_bits;
    IntSize color_image_size { ceil_div(image_size.width(), block_size), ceil_div(image_size.height(), block_size) };

    auto color_bitmap = TRY(decode_webp_chunk_VP8L_image(ImageKind::EntropyCoded, BitmapFormat::BGRx8888, color_image_size, bit_stream));

    return adopt_nonnull_own_or_enomem(new (nothrow) ColorTransform(size_bits, move(color_bitmap)));
}

ErrorOr<NonnullRefPtr<Bitmap>> ColorTransform::transform(NonnullRefPtr<Bitmap> bitmap_ref)
{
    Bitmap& bitmap = *bitmap_ref;

    for (int y = 0; y < bitmap.height(); ++y) {
        ARGB32* bitmap_scanline = bitmap.scanline(y);

        int color_y = y >> m_size_bits;
        ARGB32* color_scanline = m_color_bitmap->scanline(color_y);

        for (int x = 0; x < bitmap.width(); ++x) {
            int color_x = x >> m_size_bits;
            bitmap_scanline[x] = inverse_transform(bitmap_scanline[x], color_scanline[color_x]);
        }
    }
    return bitmap_ref;
}

ARGB32 ColorTransform::inverse_transform(ARGB32 pixel, ARGB32 transform)
{
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#51_roles_of_image_data
    // "Each ColorTransformElement 'cte' is treated as a pixel whose alpha component is 255,
    // red component is cte.red_to_blue, green component is cte.green_to_blue
    // and blue component is cte.green_to_red."
    auto transform_color = Color::from_argb(transform);
    i8 red_to_blue = static_cast<i8>(transform_color.red());
    i8 green_to_blue = static_cast<i8>(transform_color.green());
    i8 green_to_red = static_cast<i8>(transform_color.blue());

    auto pixel_color = Color::from_argb(pixel);

    // "Transformed values of red and blue components"
    int tmp_red = pixel_color.red();
    int green = pixel_color.green();
    int tmp_blue = pixel_color.blue();

    // "Applying the inverse transform is just adding the color transform deltas"
    tmp_red += ColorTransformDelta(green_to_red, green);
    tmp_blue += ColorTransformDelta(green_to_blue, green);
    tmp_blue += ColorTransformDelta(red_to_blue, tmp_red & 0xff);

    return Color(tmp_red & 0xff, green, tmp_blue & 0xff, pixel_color.alpha()).value();
}

// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#43_subtract_green_transform
class SubtractGreenTransform : public Transform {
public:
    virtual ErrorOr<NonnullRefPtr<Bitmap>> transform(NonnullRefPtr<Bitmap>) override;
};

ErrorOr<NonnullRefPtr<Bitmap>> SubtractGreenTransform::transform(NonnullRefPtr<Bitmap> bitmap)
{
    for (ARGB32& pixel : *bitmap) {
        Color color = Color::from_argb(pixel);
        u8 red = (color.red() + color.green()) & 0xff;
        u8 blue = (color.blue() + color.green()) & 0xff;
        pixel = Color(red, color.green(), blue, color.alpha()).value();
    }
    return bitmap;
}

// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#44_color_indexing_transform
class ColorIndexingTransform : public Transform {
public:
    static ErrorOr<NonnullOwnPtr<ColorIndexingTransform>> read(LittleEndianInputBitStream&, int original_width);
    virtual ErrorOr<NonnullRefPtr<Bitmap>> transform(NonnullRefPtr<Bitmap>) override;

    // For a color indexing transform, the green channel of the source image is used as the index into a palette to produce an output color.
    // If the palette is small enough, several output pixels are bundled into a single input pixel.
    // If the palette has just 2 colors, every index needs just a single bit, and the 8 bits of the green channel of one input pixel can encode 8 output pixels.
    // If the palette has 3 or 4 colors, every index needs 2 bits and every pixel can encode 4 output pixels.
    // If the palette has 5 to 16 colors, every index needs 4 bits and every pixel can encode 2 output pixels.
    // This returns how many output pixels one input pixel can encode after the color indexing transform.
    //
    // This affects all images after the color indexing transform:
    // If a webp file contains a 29x32 image and it contains a color indexing transform with a 4-color palette, then the in-memory size of all images
    // after the color indexing transform assume a bitmap size of ceil_div(29, 4)x32 = 8x32.
    // That is, the sizes of transforms after the color indexing transform are computed relative to the size 8x32,
    // the main image's meta prefix image's size (if present) is computed relative to the size 8x32,
    // the main image is 8x32, and only applying the color indexing transform resizes the image back to 29x32.
    int pixels_per_pixel() const { return m_pixels_per_pixel; }

private:
    ColorIndexingTransform(int pixels_per_pixel, int original_width, NonnullRefPtr<Bitmap> palette_bitmap)
        : m_pixels_per_pixel(pixels_per_pixel)
        , m_original_width(original_width)
        , m_palette_bitmap(palette_bitmap)
    {
    }

    int m_pixels_per_pixel;
    int m_original_width;
    NonnullRefPtr<Bitmap> m_palette_bitmap;
};

ErrorOr<NonnullOwnPtr<ColorIndexingTransform>> ColorIndexingTransform::read(LittleEndianInputBitStream& bit_stream, int original_width)
{
    // color-indexing-image =  8BIT ; color count
    //                         entropy-coded-image
    int color_table_size = TRY(bit_stream.read_bits(8)) + 1;
    dbgln_if(WEBP_DEBUG, "colorindexing color_table_size {}", color_table_size);

    IntSize palette_image_size { color_table_size, 1 };
    auto palette_bitmap = TRY(decode_webp_chunk_VP8L_image(ImageKind::EntropyCoded, BitmapFormat::BGRA8888, palette_image_size, bit_stream));

    // "When the color table is small (equal to or less than 16 colors), several pixels are bundled into a single pixel..."
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

    // "The color table is always subtraction-coded to reduce image entropy. [...]  In decoding, every final color in the color table
    //  can be obtained by adding the previous color component values by each ARGB component separately,
    //  and storing the least significant 8 bits of the result."
    for (ARGB32* pixel = palette_bitmap->begin() + 1; pixel != palette_bitmap->end(); ++pixel)
        *pixel = add_argb32(*pixel, pixel[-1]);

    return adopt_nonnull_own_or_enomem(new (nothrow) ColorIndexingTransform(pixels_per_pixel, original_width, move(palette_bitmap)));
}

ErrorOr<NonnullRefPtr<Bitmap>> ColorIndexingTransform::transform(NonnullRefPtr<Bitmap> bitmap)
{
    if (pixels_per_pixel() == 1) {
        for (ARGB32& pixel : *bitmap) {
            // "The inverse transform for the image is simply replacing the pixel values (which are indices to the color table)
            //  with the actual color table values. The indexing is done based on the green component of the ARGB color. [...]
            //  If the index is equal or larger than color_table_size, the argb color value should be set to 0x00000000 (transparent black)."
            u8 index = Color::from_argb(pixel).green();
            pixel = index < m_palette_bitmap->width() ? m_palette_bitmap->scanline(0)[index] : 0;
        }
        return bitmap;
    }

    // Pixel bundling case.
    VERIFY(ceil_div(m_original_width, pixels_per_pixel()) == bitmap->size().width());
    IntSize unbundled_size = { m_original_width, bitmap->size().height() };
    auto new_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, unbundled_size));

    unsigned bits_per_pixel = 8 / pixels_per_pixel();
    unsigned pixel_mask = (1 << bits_per_pixel) - 1;
    for (int y = 0; y < bitmap->height(); ++y) {
        ARGB32* bitmap_scanline = bitmap->scanline(y);
        ARGB32* new_bitmap_scanline = new_bitmap->scanline(y);

        for (int x = 0, new_x = 0; x < bitmap->width(); ++x, new_x += pixels_per_pixel()) {
            u8 indexes = Color::from_argb(bitmap_scanline[x]).green();

            for (int i = 0; i < pixels_per_pixel() && new_x + i < new_bitmap->width(); ++i) {
                u8 index = indexes & pixel_mask;
                new_bitmap_scanline[new_x + i] = index < m_palette_bitmap->width() ? m_palette_bitmap->scanline(0)[index] : 0;
                indexes >>= bits_per_pixel;
            }
        }
    }

    return new_bitmap;
}

}

// https://developers.google.com/speed/webp/docs/riff_container#simple_file_format_lossless
// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#7_overall_structure_of_the_format
ErrorOr<NonnullRefPtr<Bitmap>> decode_webp_chunk_VP8L_contents(VP8LHeader const& vp8l_header)
{
    FixedMemoryStream memory_stream { vp8l_header.lossless_data };
    LittleEndianInputBitStream bit_stream { MaybeOwned<Stream>(memory_stream), LittleEndianInputBitStream::UnsatisfiableReadBehavior::FillWithZero };

    // image-stream = optional-transform spatially-coded-image

    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#4_transformations
    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#72_structure_of_transforms

    auto stored_size = IntSize { vp8l_header.width, vp8l_header.height };

    // optional-transform   =  (%b1 transform optional-transform) / %b0
    u8 seen_transforms = 0;
    Vector<NonnullOwnPtr<Transform>, 4> transforms;
    while (TRY(bit_stream.read_bits(1))) {
        // transform            =  predictor-tx / color-tx / subtract-green-tx
        // transform            =/ color-indexing-tx

        TransformType transform_type = static_cast<TransformType>(TRY(bit_stream.read_bits(2)));
        dbgln_if(WEBP_DEBUG, "transform type {}", (int)transform_type);

        // "Each transform is allowed to be used only once."
        u8 mask = 1 << (int)transform_type;
        if (seen_transforms & mask)
            return Error::from_string_literal("WebPImageDecoderPlugin: transform type used multiple times");
        seen_transforms |= mask;

        // "Transform data contains the information required to apply the inverse transform and depends on the transform type."
        switch (transform_type) {
        case PREDICTOR_TRANSFORM:
            TRY(transforms.try_append(TRY(PredictorTransform::read(bit_stream, stored_size))));
            break;
        case COLOR_TRANSFORM:
            TRY(transforms.try_append(TRY(ColorTransform::read(bit_stream, stored_size))));
            break;
        case SUBTRACT_GREEN_TRANSFORM:
            TRY(transforms.try_append(TRY(try_make<SubtractGreenTransform>())));
            break;
        case COLOR_INDEXING_TRANSFORM: {
            auto color_indexing_transform = TRY(ColorIndexingTransform::read(bit_stream, stored_size.width()));

            // "After reading this transform, image_width is subsampled by width_bits. This affects the size of subsequent transforms."
            stored_size.set_width(ceil_div(stored_size.width(), color_indexing_transform->pixels_per_pixel()));

            TRY(transforms.try_append(move(color_indexing_transform)));
            break;
        }
        }
    }

    auto format = vp8l_header.is_alpha_used ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
    auto bitmap = TRY(decode_webp_chunk_VP8L_image(ImageKind::SpatiallyCoded, format, stored_size, bit_stream));

    // "The inverse transforms are applied in the reverse order that they are read from the bitstream, that is, last one first."
    for (auto const& transform : transforms.in_reverse())
        bitmap = TRY(transform->transform(bitmap));

    if (!vp8l_header.is_alpha_used)
        bitmap->strip_alpha_channel();

    return bitmap;
}

}
