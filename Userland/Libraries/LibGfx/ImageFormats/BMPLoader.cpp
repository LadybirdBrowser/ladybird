/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022, Bruno Conde <brunompconde@gmail.com>
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Try.h>
#include <AK/Vector.h>
#include <LibGfx/ImageFormats/BMPLoader.h>

namespace Gfx {

u8 const bmp_header_size = 14;
u32 const color_palette_limit = 1024;

// Compression flags
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/4e588f70-bd92-4a6f-b77f-35d0feaf7a57
struct Compression {
    enum : u32 {
        RGB = 0,
        RLE8,
        RLE4,
        BITFIELDS,
        RLE24, // doubles as JPEG for V4+, but that is unsupported
        PNG,
        ALPHABITFIELDS,
        CMYK = 11,
        CMYKRLE8,
        CMYKRLE4,
    };
};

struct DIBCore {
    // u16 for BITMAPHEADERCORE, but i32 for everything else. If the dib type is
    // BITMAPHEADERCORE, this is range checked.
    i32 width;
    i32 height;
    u16 bpp;
};

struct DIBInfo {
    u32 compression { Compression::RGB };
    u32 image_size { 0 };
    i32 horizontal_resolution { 0 };
    i32 vertical_resolution { 0 };
    u32 number_of_palette_colors { 0 };
    u32 number_of_important_palette_colors { number_of_palette_colors };

    // Introduced in the BITMAPV2INFOHEADER and would ideally be stored in the
    // DIBV2 struct, however with a compression value of BI_BITFIELDS or
    // BI_ALPHABITFIELDS, these can be specified with the Info header.
    Vector<u32> masks;
    Vector<i8> mask_shifts;
    Vector<u8> mask_sizes;
};

struct DIBOSV2 {
    u16 recording;
    u16 halftoning;
    u16 size1;
    u16 size2;
};

template<typename T>
struct Endpoint {
    T x;
    T y;
    T z;
};

}

namespace AK {

template<typename T>
struct Formatter<Gfx::Endpoint<T>> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::Endpoint<T> const& value)
    {
        return Formatter<StringView>::format(builder, ByteString::formatted("({}, {}, {})", value.x, value.y, value.z));
    }
};

}

namespace Gfx {

// CALIBRATED_RGB, sRGB, WINDOWS_COLOR_SPACE values are from
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/eb4bbd50-b3ce-4917-895c-be31f214797f
// PROFILE_LINKED, PROFILE_EMBEDDED values are from
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/3c289fe1-c42e-42f6-b125-4b5fc49a2b20
struct ColorSpace {
    enum : u32 {
        // "This value implies that endpoints and gamma values are given in the appropriate fields" in DIBV4.
        // The only valid value in v4 bmps.
        CALIBRATED_RGB = 0,

        // "Specifies that the bitmap is in sRGB color space."
        sRGB = 0x73524742, // 'sRGB'

        // "This value indicates that the bitmap is in the system default color space, sRGB."
        WINDOWS_COLOR_SPACE = 0x57696E20, // 'Win '

        // "This value indicates that bV5ProfileData points to the file name of the profile to use
        //  (gamma and endpoints values are ignored)."
        LINKED = 0x4C494E4B, // 'LINK'

        // "This value indicates that bV5ProfileData points to a memory buffer that contains the profile to be used
        //  (gamma and endpoints values are ignored)."
        EMBEDDED = 0x4D424544, // 'MBED'
    };
};

// https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv4header
struct DIBV4 {
    u32 color_space { 0 };
    Endpoint<i32> red_endpoint { 0, 0, 0 };
    Endpoint<i32> green_endpoint { 0, 0, 0 };
    Endpoint<i32> blue_endpoint { 0, 0, 0 };
    Endpoint<u32> gamma_endpoint { 0, 0, 0 };
};

// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/9fec0834-607d-427d-abd5-ab240fb0db38
struct GamutMappingIntent {
    enum : u32 {
        // "Specifies that the white point SHOULD be maintained.
        //  Typically used when logical colors MUST be matched to their nearest physical color in the destination color gamut.
        //
        //  Intent: Match
        //
        //  ICC name: Absolute Colorimetric"
        ABS_COLORIMETRIC = 8,

        // "Specifies that saturation SHOULD be maintained.
        //  Typically used for business charts and other situations in which dithering is not required.
        //
        //  Intent: Graphic
        //
        //  ICC name: Saturation"
        BUSINESS = 1,

        // "Specifies that a colorimetric match SHOULD be maintained.
        //  Typically used for graphic designs and named colors.
        //
        //  Intent: Proof
        //
        //  ICC name: Relative Colorimetric"
        GRAPHICS = 2,

        // "Specifies that contrast SHOULD be maintained.
        //  Typically used for photographs and natural images.
        //
        //  Intent: Picture
        //
        //  ICC name: Perceptual"
        IMAGES = 4,
    };
};

// https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv5header
struct DIBV5 {
    u32 intent { 0 };
    u32 profile_data { 0 };
    u32 profile_size { 0 };
};

struct DIB {
    DIBCore core;
    DIBInfo info;
    DIBOSV2 osv2;
    DIBV4 v4;
    DIBV5 v5;
};

enum class DIBType {
    Core = 0,
    OSV2Short,
    OSV2,
    Info,
    V2,
    V3,
    V4,
    V5,
};

struct BMPLoadingContext {
    enum class State {
        NotDecoded = 0,
        DIBDecoded,
        ColorTableDecoded,
        PixelDataDecoded,
        Error,
    };
    State state { State::NotDecoded };

    u8 const* file_bytes { nullptr };
    size_t file_size { 0 };
    u32 data_offset { 0 };

    bool is_included_in_ico { false };

    DIB dib;
    DIBType dib_type;

    Vector<u32> color_table;
    RefPtr<Gfx::Bitmap> bitmap;

    u32 dib_size() const
    {
        switch (dib_type) {
        case DIBType::Core:
            return 12;
        case DIBType::OSV2Short:
            return 16;
        case DIBType::OSV2:
            return 64;
        case DIBType::Info:
            return 40;
        case DIBType::V2:
            return 52;
        case DIBType::V3:
            return 56;
        case DIBType::V4:
            return 108;
        case DIBType::V5:
            return 124;
        }

        VERIFY_NOT_REACHED();
    }
};

class InputStreamer {
public:
    InputStreamer(u8 const* data, size_t size)
        : m_data_ptr(data)
        , m_size_remaining(size)
    {
    }

    u8 read_u8()
    {
        VERIFY(m_size_remaining >= 1);
        m_size_remaining--;
        return *(m_data_ptr++);
    }

    u16 read_u16()
    {
        return read_u8() | (read_u8() << 8);
    }

    u32 read_u24()
    {
        return read_u8() | (read_u8() << 8) | (read_u8() << 16);
    }

    i32 read_i32()
    {
        return static_cast<i32>(read_u16() | (read_u16() << 16));
    }

    u32 read_u32()
    {
        return read_u16() | (read_u16() << 16);
    }

    void drop_bytes(u8 num_bytes)
    {
        VERIFY(m_size_remaining >= num_bytes);
        m_size_remaining -= num_bytes;
        m_data_ptr += num_bytes;
    }

    bool at_end() const { return !m_size_remaining; }

    bool has_u8() const { return m_size_remaining >= 1; }
    bool has_u16() const { return m_size_remaining >= 2; }
    bool has_u24() const { return m_size_remaining >= 3; }
    bool has_u32() const { return m_size_remaining >= 4; }

    size_t remaining() const { return m_size_remaining; }

private:
    u8 const* m_data_ptr { nullptr };
    size_t m_size_remaining { 0 };
};

// Lookup table for distributing all possible 2-bit numbers evenly into 8-bit numbers
static u8 scaling_factors_2bit[4] = {
    0x00,
    0x55,
    0xaa,
    0xff,
};

// Lookup table for distributing all possible 3-bit numbers evenly into 8-bit numbers
static u8 scaling_factors_3bit[8] = {
    0x00,
    0x24,
    0x48,
    0x6d,
    0x91,
    0xb6,
    0xdb,
    0xff,
};

static u8 scale_masked_8bit_number(u8 number, u8 bits_set)
{
    // If there are more than 4 bit set, an easy way to scale the number is to
    // just copy the most significant bits into the least significant bits
    if (bits_set >= 4)
        return number | (number >> bits_set);
    if (!bits_set)
        return 0;
    if (bits_set == 1)
        return number ? 0xff : 0;
    if (bits_set == 2)
        return scaling_factors_2bit[number >> 6];
    return scaling_factors_3bit[number >> 5];
}

static u8 get_scaled_color(u32 data, u8 mask_size, i8 mask_shift)
{
    // A negative mask_shift indicates we actually need to left shift
    // the result in order to get out a valid 8-bit color (for example, the blue
    // value in an RGB555 encoding is XXXBBBBB, which needs to be shifted to the
    // left by 3, hence it would have a "mask_shift" value of -3).
    if (mask_shift < 0)
        return scale_masked_8bit_number(data << -mask_shift, mask_size);
    return scale_masked_8bit_number(data >> mask_shift, mask_size);
}

// Scales an 8-bit number with "mask_size" bits set (and "8 - mask_size" bits
//   ignored). This function scales the number appropriately over the entire
//   256 value color spectrum.
// Note that a much simpler scaling can be done by simple bit shifting. If you
//   just ignore the bottom 8-mask_size bits, then you get *close*. However,
//   consider, as an example, a 5 bit number (so the bottom 3 bits are ignored).
//   The purest white you could get is 0xf8, which is 248 in RGB-land. We need
//   to scale the values in order to reach the proper value of 255.
static u32 int_to_scaled_rgb(BMPLoadingContext& context, u32 data)
{
    dbgln_if(BMP_DEBUG, "DIB info sizes before access: #masks={}, #mask_sizes={}, #mask_shifts={}",
        context.dib.info.masks.size(),
        context.dib.info.mask_sizes.size(),
        context.dib.info.mask_shifts.size());

    u8 r = get_scaled_color(data & context.dib.info.masks[0], context.dib.info.mask_sizes[0], context.dib.info.mask_shifts[0]);
    u8 g = get_scaled_color(data & context.dib.info.masks[1], context.dib.info.mask_sizes[1], context.dib.info.mask_shifts[1]);
    u8 b = get_scaled_color(data & context.dib.info.masks[2], context.dib.info.mask_sizes[2], context.dib.info.mask_shifts[2]);
    u32 color = (r << 16) | (g << 8) | b;

    if (context.dib.info.masks.size() == 4) {
        // The bitmap has an alpha mask
        u8 a = get_scaled_color(data & context.dib.info.masks[3], context.dib.info.mask_sizes[3], context.dib.info.mask_shifts[3]);
        color |= (a << 24);
    } else {
        color |= 0xff000000;
    }

    return color;
}

static void populate_dib_mask_info_if_needed(BMPLoadingContext& context)
{
    if (context.dib.info.masks.is_empty())
        return;

    // Mask shift is the number of right shifts needed to align the MSb of the
    // mask to the MSb of the LSB. Note that this can be a negative number.
    // Mask size is the number of set bits in the mask. This is required for
    // color scaling (for example, ensuring that a 4-bit color value spans the
    // entire 256 value color spectrum.
    auto& masks = context.dib.info.masks;
    auto& mask_shifts = context.dib.info.mask_shifts;
    auto& mask_sizes = context.dib.info.mask_sizes;

    if (!mask_shifts.is_empty() && !mask_sizes.is_empty())
        return;

    VERIFY(mask_shifts.is_empty() && mask_sizes.is_empty());

    mask_shifts.ensure_capacity(masks.size());
    mask_sizes.ensure_capacity(masks.size());

    for (size_t i = 0; i < masks.size(); ++i) {
        u32 mask = masks[i];
        if (!mask) {
            mask_shifts.append(0);
            mask_sizes.append(0);
            continue;
        }
        int trailing_zeros = count_trailing_zeroes(mask);
        // If mask is exactly `0xFFFFFFFF`, then we might try to count the trailing zeros of 0x00000000 here, so we need the safe version:
        int size = count_trailing_zeroes_safe(~(mask >> trailing_zeros));
        if (size > 8) {
            // Drop lowest bits if mask is longer than 8 bits.
            trailing_zeros += size - 8;
            size = 8;
        }
        mask_shifts.append(size + trailing_zeros - 8);
        mask_sizes.append(size);
    }
}

static bool check_for_invalid_bitmask_combinations(BMPLoadingContext& context)
{
    auto& bpp = context.dib.core.bpp;
    auto& compression = context.dib.info.compression;

    if (compression == Compression::ALPHABITFIELDS && context.dib_type != DIBType::Info)
        return false;

    switch (context.dib_type) {
    case DIBType::Core:
        if (bpp == 2 || bpp == 16 || bpp == 32)
            return false;
        break;
    case DIBType::Info:
        switch (compression) {
        case Compression::BITFIELDS:
        case Compression::ALPHABITFIELDS:
            if (bpp != 16 && bpp != 32)
                return false;
            break;
        case Compression::RGB:
            break;
        case Compression::RLE8:
            if (bpp > 8)
                return false;
            break;
        case Compression::RLE4:
            if (bpp > 4)
                return false;
            break;
        default:
            // Other compressions are not officially supported.
            // Technically, we could even drop ALPHABITFIELDS.
            return false;
        }
        break;
    case DIBType::OSV2Short:
    case DIBType::OSV2:
    case DIBType::V2:
    case DIBType::V3:
    case DIBType::V4:
    case DIBType::V5:
        if (compression == Compression::BITFIELDS && bpp != 16 && bpp != 32)
            return false;
        break;
    }

    return true;
}

static bool set_dib_bitmasks(BMPLoadingContext& context, InputStreamer& streamer)
{
    if (!check_for_invalid_bitmask_combinations(context))
        return false;

    auto& bpp = context.dib.core.bpp;
    if (bpp <= 8 || bpp == 24)
        return true;

    auto& compression = context.dib.info.compression;
    auto& type = context.dib_type;

    if (type > DIBType::OSV2 && bpp == 16 && compression == Compression::RGB) {
        context.dib.info.masks.extend({ 0x7c00, 0x03e0, 0x001f });
        context.dib.info.mask_shifts.extend({ 7, 2, -3 });
        context.dib.info.mask_sizes.extend({ 5, 5, 5 });
    } else if (type == DIBType::Info && (compression == Compression::BITFIELDS || compression == Compression::ALPHABITFIELDS)) {
        // Consume the extra BITFIELDS bytes
        auto number_of_mask_fields = compression == Compression::ALPHABITFIELDS ? 4 : 3;

        for (auto i = 0; i < number_of_mask_fields; i++) {
            if (!streamer.has_u32())
                return false;
            context.dib.info.masks.append(streamer.read_u32());
        }
    }

    populate_dib_mask_info_if_needed(context);
    return true;
}

static ErrorOr<void> decode_bmp_header(BMPLoadingContext& context)
{
    if (!context.file_bytes || context.file_size < bmp_header_size) {
        dbgln_if(BMP_DEBUG, "Missing BMP header");
        context.state = BMPLoadingContext::State::Error;
        return Error::from_string_literal("Missing BMP header");
    }

    InputStreamer streamer(context.file_bytes, bmp_header_size);

    u16 header = streamer.read_u16();
    if (header != 0x4d42) {
        dbgln_if(BMP_DEBUG, "BMP has invalid magic header number: {:#04x}", header);
        context.state = BMPLoadingContext::State::Error;
        return Error::from_string_literal("BMP has invalid magic header number");
    }

    // The reported size of the file in the header is actually not important
    // for decoding the file. Some specifications say that this value should
    // be the size of the header instead, so we just rely on the known file
    // size, instead of a possibly-correct-but-also-possibly-incorrect reported
    // value of the file size.
    streamer.drop_bytes(4);

    // Ignore reserved bytes
    streamer.drop_bytes(4);
    context.data_offset = streamer.read_u32();
    if (context.data_offset >= context.file_size) {
        dbgln_if(BMP_DEBUG, "BMP has invalid data offset: {}", context.data_offset);
        context.state = BMPLoadingContext::State::Error;
        return Error::from_string_literal("BMP has invalid data offset");
    }

    if constexpr (BMP_DEBUG) {
        dbgln("BMP file size: {}", context.file_size);
        dbgln("BMP data offset: {}", context.data_offset);
    }

    if (context.data_offset >= context.file_size) {
        dbgln_if(BMP_DEBUG, "BMP data offset is beyond file end?!");
        return Error::from_string_literal("BMP data offset is beyond file end");
    }

    return {};
}

static bool decode_bmp_core_dib(BMPLoadingContext& context, InputStreamer& streamer)
{
    auto& core = context.dib.core;

    // The width and height are u16 fields in the actual BITMAPCOREHEADER format.
    if (context.dib_type == DIBType::Core) {
        core.width = streamer.read_u16();
        core.height = streamer.read_u16();
    } else {
        core.width = streamer.read_i32();
        core.height = streamer.read_i32();
    }

    if (core.width < 0) {
        dbgln("BMP has a negative width: {}", core.width);
        return false;
    }

    auto color_planes = streamer.read_u16();
    if (color_planes != 1) {
        dbgln("BMP has an invalid number of color planes: {}", color_planes);
        return false;
    }

    core.bpp = streamer.read_u16();
    switch (core.bpp) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
    case 24:
    case 32:
        break;
    default:
        dbgln("BMP has an invalid bpp: {}", core.bpp);
        context.state = BMPLoadingContext::State::Error;
        return false;
    }

    if constexpr (BMP_DEBUG) {
        dbgln("BMP width: {}", core.width);
        dbgln("BMP height: {}", core.height);
        dbgln("BMP bits_per_pixel: {}", core.bpp);
    }

    return true;
}

ALWAYS_INLINE static bool is_supported_compression_format(BMPLoadingContext& context, u32 compression)
{
    return compression == Compression::RGB || compression == Compression::BITFIELDS
        || compression == Compression::ALPHABITFIELDS || compression == Compression::RLE8
        || compression == Compression::RLE4 || (compression == Compression::RLE24 && context.dib_type <= DIBType::OSV2);
}

static bool decode_bmp_osv2_dib(BMPLoadingContext& context, InputStreamer& streamer, bool short_variant = false)
{
    auto& core = context.dib.core;

    core.width = streamer.read_u32();
    core.height = streamer.read_u32();

    if (core.width < 0) {
        dbgln("BMP has a negative width: {}", core.width);
        return false;
    }

    auto color_planes = streamer.read_u16();
    if (color_planes != 1) {
        dbgln("BMP has an invalid number of color planes: {}", color_planes);
        return false;
    }

    core.bpp = streamer.read_u16();
    switch (core.bpp) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 24:
        break;
    default:
        // OS/2 didn't expect 16- or 32-bpp to be popular.
        dbgln("BMP has an invalid bpp: {}", core.bpp);
        context.state = BMPLoadingContext::State::Error;
        return false;
    }

    if constexpr (BMP_DEBUG) {
        dbgln("BMP width: {}", core.width);
        dbgln("BMP height: {}", core.height);
        dbgln("BMP bits_per_pixel: {}", core.bpp);
    }

    if (short_variant)
        return true;

    auto& info = context.dib.info;
    auto& osv2 = context.dib.osv2;

    info.compression = streamer.read_u32();
    info.image_size = streamer.read_u32();
    info.horizontal_resolution = streamer.read_u32();
    info.vertical_resolution = streamer.read_u32();
    info.number_of_palette_colors = streamer.read_u32();
    info.number_of_important_palette_colors = streamer.read_u32();

    if (!is_supported_compression_format(context, info.compression)) {
        dbgln("BMP has unsupported compression value: {}", info.compression);
        return false;
    }

    if (info.number_of_palette_colors > color_palette_limit || info.number_of_important_palette_colors > color_palette_limit) {
        dbgln("BMP header indicates too many palette colors: {}", info.number_of_palette_colors);
        return false;
    }

    // Units (2) + reserved (2)
    streamer.drop_bytes(4);

    osv2.recording = streamer.read_u16();
    osv2.halftoning = streamer.read_u16();
    osv2.size1 = streamer.read_u32();
    osv2.size2 = streamer.read_u32();

    // ColorEncoding (4) + Identifier (4)
    streamer.drop_bytes(8);

    if constexpr (BMP_DEBUG) {
        dbgln("BMP compression: {}", info.compression);
        dbgln("BMP image size: {}", info.image_size);
        dbgln("BMP horizontal res: {}", info.horizontal_resolution);
        dbgln("BMP vertical res: {}", info.vertical_resolution);
        dbgln("BMP colors: {}", info.number_of_palette_colors);
        dbgln("BMP important colors: {}", info.number_of_important_palette_colors);
    }

    return true;
}

static bool decode_bmp_info_dib(BMPLoadingContext& context, InputStreamer& streamer)
{
    if (!decode_bmp_core_dib(context, streamer))
        return false;

    auto& info = context.dib.info;

    auto compression = streamer.read_u32();
    info.compression = compression;
    if (!is_supported_compression_format(context, compression)) {
        dbgln("BMP has unsupported compression value: {}", compression);
        return false;
    }

    info.image_size = streamer.read_u32();
    info.horizontal_resolution = streamer.read_i32();
    info.vertical_resolution = streamer.read_i32();
    info.number_of_palette_colors = streamer.read_u32();
    info.number_of_important_palette_colors = streamer.read_u32();

    if (info.number_of_palette_colors > color_palette_limit || info.number_of_important_palette_colors > color_palette_limit) {
        dbgln("BMP header indicates too many palette colors: {}", info.number_of_palette_colors);
        return false;
    }

    if (info.number_of_important_palette_colors == 0)
        info.number_of_important_palette_colors = info.number_of_palette_colors;

    if constexpr (BMP_DEBUG) {
        dbgln("BMP compression: {}", info.compression);
        dbgln("BMP image size: {}", info.image_size);
        dbgln("BMP horizontal res: {}", info.horizontal_resolution);
        dbgln("BMP vertical res: {}", info.vertical_resolution);
        dbgln("BMP colors: {}", info.number_of_palette_colors);
        dbgln("BMP important colors: {}", info.number_of_important_palette_colors);
    }

    return true;
}

static bool decode_bmp_v2_dib(BMPLoadingContext& context, InputStreamer& streamer)
{
    if (!decode_bmp_info_dib(context, streamer))
        return false;

    context.dib.info.masks.append(streamer.read_u32());
    context.dib.info.masks.append(streamer.read_u32());
    context.dib.info.masks.append(streamer.read_u32());

    if constexpr (BMP_DEBUG) {
        dbgln("BMP red mask: {:#08x}", context.dib.info.masks[0]);
        dbgln("BMP green mask: {:#08x}", context.dib.info.masks[1]);
        dbgln("BMP blue mask: {:#08x}", context.dib.info.masks[2]);
    }

    return true;
}

static bool decode_bmp_v3_dib(BMPLoadingContext& context, InputStreamer& streamer)
{
    if (!decode_bmp_v2_dib(context, streamer))
        return false;

    // There is zero documentation about when alpha masks actually get applied.
    // Well, there's some, but it's not even close to comprehensive. So, this is
    // in no way based off of any spec, it's simply based off of the BMP test
    // suite results.
    if (context.dib.info.compression == Compression::ALPHABITFIELDS) {
        context.dib.info.masks.append(streamer.read_u32());
        dbgln_if(BMP_DEBUG, "BMP alpha mask: {:#08x}", context.dib.info.masks[3]);
    } else if (context.dib_size() >= 56 && context.dib.core.bpp >= 16) {
        auto mask = streamer.read_u32();
        if ((context.dib.core.bpp == 32 && mask != 0) || context.dib.core.bpp == 16) {
            context.dib.info.masks.append(mask);
            dbgln_if(BMP_DEBUG, "BMP alpha mask: {:#08x}", mask);
        } else {
            dbgln_if(BMP_DEBUG, "BMP alpha mask (ignored): {:#08x}", mask);
        }
    } else {
        streamer.drop_bytes(4);
        dbgln_if(BMP_DEBUG, "BMP alpha mask skipped");
    }

    return true;
}

static bool decode_bmp_v4_dib(BMPLoadingContext& context, InputStreamer& streamer)
{
    if (!decode_bmp_v3_dib(context, streamer))
        return false;

    auto& v4 = context.dib.v4;
    v4.color_space = streamer.read_u32();
    v4.red_endpoint = { streamer.read_i32(), streamer.read_i32(), streamer.read_i32() };
    v4.green_endpoint = { streamer.read_i32(), streamer.read_i32(), streamer.read_i32() };
    v4.blue_endpoint = { streamer.read_i32(), streamer.read_i32(), streamer.read_i32() };
    v4.gamma_endpoint = { streamer.read_u32(), streamer.read_u32(), streamer.read_u32() };

    if constexpr (BMP_DEBUG) {
        dbgln("BMP color space: {}", v4.color_space);
        dbgln("BMP red endpoint: {}", v4.red_endpoint);
        dbgln("BMP green endpoint: {}", v4.green_endpoint);
        dbgln("BMP blue endpoint: {}", v4.blue_endpoint);
        dbgln("BMP gamma endpoint: {}", v4.gamma_endpoint);
    }

    return true;
}

static bool decode_bmp_v5_dib(BMPLoadingContext& context, InputStreamer& streamer)
{
    if (!decode_bmp_v4_dib(context, streamer))
        return false;

    auto& v5 = context.dib.v5;
    v5.intent = streamer.read_u32();
    v5.profile_data = streamer.read_u32();
    v5.profile_size = streamer.read_u32();
    streamer.drop_bytes(4); // Ignore reserved field.

    if constexpr (BMP_DEBUG) {
        dbgln("BMP intent: {}", v5.intent);
        dbgln("BMP profile data: {}", v5.profile_data);
        dbgln("BMP profile size: {}", v5.profile_size);
    }

    return true;
}

static ErrorOr<void> decode_bmp_dib(BMPLoadingContext& context)
{
    if (context.state == BMPLoadingContext::State::Error)
        return Error::from_string_literal("Error before starting decode_bmp_dib");

    if (context.state >= BMPLoadingContext::State::DIBDecoded)
        return {};

    if (!context.is_included_in_ico)
        TRY(decode_bmp_header(context));

    u8 header_size = context.is_included_in_ico ? 0 : bmp_header_size;

    if (context.file_size < header_size + 4u)
        return Error::from_string_literal("File size too short");

    InputStreamer streamer(context.file_bytes + header_size, 4);

    u64 dib_size = streamer.read_u32();

    if (context.file_size < header_size + dib_size)
        return Error::from_string_literal("File size too short");

    if (!context.is_included_in_ico && (context.data_offset < header_size + dib_size)) {
        dbgln("Shenanigans! BMP pixel data and header usually don't overlap.");
        return Error::from_string_literal("BMP pixel data and header usually don't overlap");
    }

    // NOTE: If this is a headless BMP (embedded on ICO files), then we can only infer the data_offset after we know the data table size.
    // We are also assuming that no Extra bit masks are present
    u64 dib_offset = dib_size;
    if (!context.is_included_in_ico) {
        if (context.data_offset < header_size + 4u)
            return Error::from_string_literal("Data offset too small");

        dib_offset = context.data_offset - header_size - 4;
    }

    if (dib_offset + header_size + 4 >= context.file_size)
        return Error::from_string_literal("DIB too large");

    streamer = InputStreamer(context.file_bytes + header_size + 4, dib_offset);

    dbgln_if(BMP_DEBUG, "BMP dib size: {}", dib_size);

    bool error = false;

    if (dib_size == 12) {
        context.dib_type = DIBType::Core;
        if (!decode_bmp_core_dib(context, streamer))
            error = true;
    } else if (dib_size == 64) {
        context.dib_type = DIBType::OSV2;
        if (!decode_bmp_osv2_dib(context, streamer))
            error = true;
    } else if (dib_size == 16) {
        context.dib_type = DIBType::OSV2Short;
        if (!decode_bmp_osv2_dib(context, streamer, true))
            error = true;
    } else if (dib_size == 40) {
        context.dib_type = DIBType::Info;
        if (!decode_bmp_info_dib(context, streamer))
            error = true;
    } else if (dib_size == 52) {
        context.dib_type = DIBType::V2;
        if (!decode_bmp_v2_dib(context, streamer))
            error = true;
    } else if (dib_size == 56) {
        context.dib_type = DIBType::V3;
        if (!decode_bmp_v3_dib(context, streamer))
            error = true;
    } else if (dib_size == 108) {
        context.dib_type = DIBType::V4;
        if (!decode_bmp_v4_dib(context, streamer))
            error = true;
    } else if (dib_size == 124) {
        context.dib_type = DIBType::V5;
        if (!decode_bmp_v5_dib(context, streamer))
            error = true;
    } else {
        dbgln("Unsupported BMP DIB size: {}", dib_size);
        error = true;
    }

    switch (context.dib.info.compression) {
    case Compression::RGB:
    case Compression::RLE8:
    case Compression::RLE4:
    case Compression::BITFIELDS:
    case Compression::RLE24:
    case Compression::PNG:
    case Compression::ALPHABITFIELDS:
    case Compression::CMYK:
    case Compression::CMYKRLE8:
    case Compression::CMYKRLE4:
        break;
    default:
        error = true;
    }

    if (!error && !set_dib_bitmasks(context, streamer))
        error = true;

    if (error) {
        dbgln("BMP has an invalid DIB");
        context.state = BMPLoadingContext::State::Error;
        return Error::from_string_literal("BMP has an invalid DIB");
    }

    // NOTE: If this is a headless BMP (included on ICOns), the data_offset is set based on the number_of_palette_colors found on the DIB header
    if (context.is_included_in_ico) {
        if (context.dib.core.bpp > 8)
            context.data_offset = dib_size;
        else {
            auto bytes_per_color = context.dib_type == DIBType::Core ? 3 : 4;
            u32 max_colors = 1 << context.dib.core.bpp;
            auto size_of_color_table = (context.dib.info.number_of_palette_colors > 0 ? context.dib.info.number_of_palette_colors : max_colors) * bytes_per_color;
            context.data_offset = dib_size + size_of_color_table;
        }
    }

    if (context.data_offset >= context.file_size) {
        dbgln_if(BMP_DEBUG, "BMP has invalid data offset: {}", context.data_offset);
        context.state = BMPLoadingContext::State::Error;
        return Error::from_string_literal("BMP has invalid data offset");
    }

    context.state = BMPLoadingContext::State::DIBDecoded;

    return {};
}

static ErrorOr<void> decode_bmp_color_table(BMPLoadingContext& context)
{
    if (context.state == BMPLoadingContext::State::Error)
        return Error::from_string_literal("Error before starting decode_bmp_color_table");

    if (context.state >= BMPLoadingContext::State::ColorTableDecoded)
        return {};

    if (context.dib.core.bpp > 8) {
        context.state = BMPLoadingContext::State::ColorTableDecoded;
        return {};
    }

    auto bytes_per_color = context.dib_type == DIBType::Core ? 3 : 4;
    u32 max_colors = 1 << context.dib.core.bpp;

    u8 header_size = !context.is_included_in_ico ? bmp_header_size : 0;
    VERIFY(context.data_offset >= header_size + context.dib_size());

    u32 size_of_color_table;
    if (!context.is_included_in_ico) {
        size_of_color_table = context.data_offset - header_size - context.dib_size();
    } else {
        size_of_color_table = (context.dib.info.number_of_palette_colors > 0 ? context.dib.info.number_of_palette_colors : max_colors) * bytes_per_color;
    }

    if (context.dib_type <= DIBType::OSV2) {
        // Partial color tables are not supported, so the space of the color
        // table must be at least enough for the maximum amount of colors
        if (size_of_color_table < 3 * max_colors) {
            // This is against the spec, but most viewers process it anyways
            dbgln("BMP with CORE header does not have enough colors. Has: {}, expected: {}", size_of_color_table, (3 * max_colors));
        }
    }

    InputStreamer streamer(context.file_bytes + header_size + context.dib_size(), size_of_color_table);
    for (u32 i = 0; !streamer.at_end() && i < max_colors; ++i) {
        if (bytes_per_color == 4) {
            if (!streamer.has_u32())
                return Error::from_string_literal("Cannot read 32 bits");
            context.color_table.append(streamer.read_u32() | 0xff'00'00'00);
        } else {
            if (!streamer.has_u24())
                return Error::from_string_literal("Cannot read 24 bits");
            context.color_table.append(streamer.read_u24() | 0xff'00'00'00);
        }
    }

    context.state = BMPLoadingContext::State::ColorTableDecoded;

    return {};
}

struct RLEState {
    enum : u8 {
        PixelCount = 0,
        PixelValue,
        Meta, // Represents just consuming a null byte, which indicates something special
    };
};

static ErrorOr<void> uncompress_bmp_rle_data(BMPLoadingContext& context, ByteBuffer& buffer)
{
    // RLE-compressed images cannot be stored top-down
    if (context.dib.core.height < 0) {
        dbgln_if(BMP_DEBUG, "BMP is top-down and RLE compressed");
        context.state = BMPLoadingContext::State::Error;
        return Error::from_string_literal("BMP is top-down and RLE compressed");
    }

    InputStreamer streamer(context.file_bytes + context.data_offset, context.file_size - context.data_offset);

    auto compression = context.dib.info.compression;

    u32 total_rows = static_cast<u32>(context.dib.core.height);
    u32 total_columns = round_up_to_power_of_two(static_cast<u32>(context.dib.core.width), 4);
    u32 column = 0;
    u32 row = 0;
    auto currently_consuming = RLEState::PixelCount;
    i16 pixel_count = 0;

    // ByteBuffer asserts that allocating the memory never fails.
    // FIXME: ByteBuffer should return either RefPtr<> or Optional<>.
    // Decoding the RLE data on-the-fly might actually be faster, and avoids this topic entirely.
    u32 buffer_size;
    if (compression == Compression::RLE24) {
        buffer_size = total_rows * round_up_to_power_of_two(total_columns, 4) * 4;
    } else {
        buffer_size = total_rows * round_up_to_power_of_two(total_columns, 4);
    }
    if (buffer_size > 300 * MiB) {
        dbgln("Suspiciously large amount of RLE data");
        return Error::from_string_literal("Suspiciously large amount of RLE data");
    }
    auto buffer_result = ByteBuffer::create_zeroed(buffer_size);
    if (buffer_result.is_error()) {
        dbgln("Not enough memory for buffer allocation");
        return buffer_result.release_error();
    }
    buffer = buffer_result.release_value();

    // Avoid as many if statements as possible by pulling out
    // compression-dependent actions into separate lambdas
    Function<u32()> get_buffer_index;
    Function<ErrorOr<void>(u32, bool)> set_byte;
    Function<ErrorOr<u32>()> read_byte;

    if (compression == Compression::RLE8) {
        get_buffer_index = [&]() -> u32 { return row * total_columns + column; };
    } else if (compression == Compression::RLE4) {
        get_buffer_index = [&]() -> u32 { return (row * total_columns + column) / 2; };
    } else {
        get_buffer_index = [&]() -> u32 { return (row * total_columns + column) * 3; };
    }

    if (compression == Compression::RLE8) {
        set_byte = [&](u32 color, bool) -> ErrorOr<void> {
            if (column >= total_columns) {
                column = 0;
                row++;
            }
            auto index = get_buffer_index();
            if (index >= buffer.size()) {
                dbgln("BMP has badly-formatted RLE data");
                return Error::from_string_literal("BMP has badly-formatted RLE data");
            }
            buffer[index] = color;
            column++;
            return {};
        };
    } else if (compression == Compression::RLE24) {
        set_byte = [&](u32 color, bool) -> ErrorOr<void> {
            if (column >= total_columns) {
                column = 0;
                row++;
            }
            auto index = get_buffer_index();
            if (index + 3 >= buffer.size()) {
                dbgln("BMP has badly-formatted RLE data");
                return Error::from_string_literal("BMP has badly-formatted RLE data");
            }
            ((u32&)buffer[index]) = color;
            column++;
            return {};
        };
    } else {
        set_byte = [&](u32 byte, bool rle4_set_second_nibble) -> ErrorOr<void> {
            if (column >= total_columns) {
                column = 0;
                row++;
            }

            u32 index = get_buffer_index();
            if (index >= buffer.size() || (rle4_set_second_nibble && index + 1 >= buffer.size())) {
                dbgln("BMP has badly-formatted RLE data");
                return Error::from_string_literal("BMP has badly-formatted RLE data");
            }

            if (column % 2) {
                buffer[index] |= byte >> 4;
                if (rle4_set_second_nibble) {
                    buffer[index + 1] |= byte << 4;
                    column++;
                }
            } else {
                if (rle4_set_second_nibble) {
                    buffer[index] = byte;
                    column++;
                } else {
                    buffer[index] |= byte & 0xf0;
                }
            }

            column++;
            return {};
        };
    }

    if (compression == Compression::RLE24) {
        read_byte = [&]() -> ErrorOr<u32> {
            if (!streamer.has_u24()) {
                dbgln("BMP has badly-formatted RLE data");
                return Error::from_string_literal("BMP has badly-formatted RLE data");
            }
            return streamer.read_u24();
        };
    } else {
        read_byte = [&]() -> ErrorOr<u32> {
            if (!streamer.has_u8()) {
                dbgln("BMP has badly-formatted RLE data");
                return Error::from_string_literal("BMP has badly-formatted RLE data");
            }
            return streamer.read_u8();
        };
    }

    while (true) {
        u32 byte;

        switch (currently_consuming) {
        case RLEState::PixelCount:
            if (!streamer.has_u8())
                return Error::from_string_literal("Cannot read 8 bits");
            byte = streamer.read_u8();
            if (!byte) {
                currently_consuming = RLEState::Meta;
            } else {
                pixel_count = byte;
                currently_consuming = RLEState::PixelValue;
            }
            break;
        case RLEState::PixelValue:
            byte = TRY(read_byte());
            for (u16 i = 0; i < pixel_count; ++i) {
                if (compression != Compression::RLE4) {
                    TRY(set_byte(byte, true));
                } else {
                    TRY(set_byte(byte, i != pixel_count - 1));
                    i++;
                }
            }

            currently_consuming = RLEState::PixelCount;
            break;
        case RLEState::Meta:
            if (!streamer.has_u8())
                return Error::from_string_literal("Cannot read 8 bits");
            byte = streamer.read_u8();
            if (!byte) {
                column = 0;
                row++;
                currently_consuming = RLEState::PixelCount;
                continue;
            }
            if (byte == 1)
                return {};
            if (byte == 2) {
                if (!streamer.has_u8())
                    return Error::from_string_literal("Cannot read 8 bits");
                u8 offset_x = streamer.read_u8();
                if (!streamer.has_u8())
                    return Error::from_string_literal("Cannot read 8 bits");
                u8 offset_y = streamer.read_u8();
                column += offset_x;
                if (column >= total_columns) {
                    column -= total_columns;
                    row++;
                }
                row += offset_y;
                currently_consuming = RLEState::PixelCount;
                continue;
            }

            // Consume literal bytes
            pixel_count = byte;
            i16 i = byte;

            while (i >= 1) {
                byte = TRY(read_byte());
                TRY(set_byte(byte, i != 1));
                i--;
                if (compression == Compression::RLE4)
                    i--;
            }

            // Optionally consume a padding byte
            if (compression != Compression::RLE4) {
                if (pixel_count % 2) {
                    if (!streamer.has_u8())
                        return Error::from_string_literal("Cannot read 8 bits");
                    byte = streamer.read_u8();
                }
            } else {
                if (((pixel_count + 1) / 2) % 2) {
                    if (!streamer.has_u8())
                        return Error::from_string_literal("Cannot read 8 bits");
                    byte = streamer.read_u8();
                }
            }
            currently_consuming = RLEState::PixelCount;
            break;
        }
    }

    VERIFY_NOT_REACHED();
}

static ErrorOr<void> decode_bmp_pixel_data(BMPLoadingContext& context)
{
    if (context.state == BMPLoadingContext::State::Error)
        return Error::from_string_literal("Error before starting decode_bmp_pixel_data");

    if (context.state <= BMPLoadingContext::State::ColorTableDecoded)
        TRY(decode_bmp_color_table(context));

    u16 const bits_per_pixel = context.dib.core.bpp;

    BitmapFormat format = [&]() -> BitmapFormat {
        // NOTE: If this is an BMP included in an ICO, the bitmap format will be converted to BGRA8888.
        //       This is because images with less than 32 bits of color depth follow a particular format:
        //       the image is encoded with a color mask (the "XOR mask") together with an opacity mask (the "AND mask") of 1 bit per pixel.
        //       The height of the encoded image must be exactly twice the real height, before both masks are combined.
        //       Bitmaps have no knowledge of this format as they do not store extra rows for the AND mask.
        if (context.is_included_in_ico)
            return BitmapFormat::BGRA8888;

        switch (bits_per_pixel) {
        case 1:
        case 2:
        case 4:
        case 8:
            return BitmapFormat::BGRx8888;
        case 16:
            if (context.dib.info.masks.size() == 4)
                return BitmapFormat::BGRA8888;
            return BitmapFormat::BGRx8888;
        case 24:
            return BitmapFormat::RGBx8888;
        case 32:
            return BitmapFormat::BGRA8888;
        default:
            return BitmapFormat::Invalid;
        }
    }();

    if (format == BitmapFormat::Invalid) {
        dbgln("BMP has invalid bpp of {}", bits_per_pixel);
        context.state = BMPLoadingContext::State::Error;
        return Error::from_string_literal("BMP has invalid bpp");
    }

    u32 const width = abs(context.dib.core.width);
    u32 const height = !context.is_included_in_ico ? abs(context.dib.core.height) : (abs(context.dib.core.height) / 2);

    context.bitmap = TRY(Bitmap::create(format, { static_cast<int>(width), static_cast<int>(height) }));

    ByteBuffer rle_buffer;
    ReadonlyBytes bytes { context.file_bytes + context.data_offset, context.file_size - context.data_offset };

    if (context.dib.info.compression == Compression::RLE4 || context.dib.info.compression == Compression::RLE8
        || context.dib.info.compression == Compression::RLE24) {
        TRY(uncompress_bmp_rle_data(context, rle_buffer));
        bytes = rle_buffer.bytes();
    }

    InputStreamer streamer(bytes.data(), bytes.size());

    auto process_row_padding = [&](u8 const consumed) -> ErrorOr<void> {
        // Calculate padding
        u8 remaining = consumed % 4;
        u8 bytes_to_drop = remaining == 0 ? 0 : 4 - remaining;

        if (streamer.remaining() < bytes_to_drop)
            return Error::from_string_literal("Not enough bytes available to drop");
        streamer.drop_bytes(bytes_to_drop);

        return {};
    };

    auto process_row = [&](u32 row) -> ErrorOr<void> {
        u32 space_remaining_before_consuming_row = streamer.remaining();

        for (u32 column = 0; column < width;) {
            switch (bits_per_pixel) {
            case 1: {
                if (!streamer.has_u8())
                    return Error::from_string_literal("Cannot read 8 bits");
                u8 byte = streamer.read_u8();
                u8 mask = 8;
                while (column < width && mask > 0) {
                    mask -= 1;
                    size_t color_idx = (byte >> mask) & 0x1;
                    if (color_idx >= context.color_table.size())
                        return Error::from_string_literal("Invalid color table index");
                    auto color = context.color_table[color_idx];
                    context.bitmap->scanline(row)[column++] = color;
                }
                break;
            }
            case 2: {
                if (!streamer.has_u8())
                    return Error::from_string_literal("Cannot read 8 bits");
                u8 byte = streamer.read_u8();
                u8 mask = 8;
                while (column < width && mask > 0) {
                    mask -= 2;
                    size_t color_idx = (byte >> mask) & 0x3;
                    if (color_idx >= context.color_table.size())
                        return Error::from_string_literal("Invalid color table index");
                    auto color = context.color_table[color_idx];
                    context.bitmap->scanline(row)[column++] = color;
                }
                break;
            }
            case 4: {
                if (!streamer.has_u8()) {
                    return Error::from_string_literal("Cannot read 8 bits");
                }
                u8 byte = streamer.read_u8();

                u32 high_color_idx = (byte >> 4) & 0xf;
                u32 low_color_idx = byte & 0xf;

                if (high_color_idx >= context.color_table.size() || low_color_idx >= context.color_table.size())
                    return Error::from_string_literal("Invalid color table index");
                auto high_color = context.color_table[high_color_idx];
                auto low_color = context.color_table[low_color_idx];
                context.bitmap->scanline(row)[column++] = high_color;
                if (column < width) {
                    context.bitmap->scanline(row)[column++] = low_color;
                }
                break;
            }
            case 8: {
                if (!streamer.has_u8())
                    return Error::from_string_literal("Cannot read 8 bits");

                u8 byte = streamer.read_u8();
                if (byte >= context.color_table.size())
                    return Error::from_string_literal("Invalid color table index");
                auto color = context.color_table[byte];
                context.bitmap->scanline(row)[column++] = color;
                break;
            }
            case 16: {
                if (!streamer.has_u16())
                    return Error::from_string_literal("Cannot read 16 bits");
                context.bitmap->scanline(row)[column++] = int_to_scaled_rgb(context, streamer.read_u16());
                break;
            }
            case 24: {
                if (!streamer.has_u24())
                    return Error::from_string_literal("Cannot read 24 bits");

                u32 pixel = streamer.read_u24();
                u8 b = (pixel & 0xFF0000) >> 16;
                u8 g = (pixel & 0x00FF00) >> 8;
                u8 r = (pixel & 0x0000FF);

                u32 rgbx_pixel = (r << 16) | (g << 8) | b;
                context.bitmap->scanline(row)[column++] = rgbx_pixel;
                break;
            }
            case 32:
                if (!streamer.has_u32())
                    return Error::from_string_literal("Cannot read 32 bits");
                if (context.dib.info.masks.is_empty()) {
                    context.bitmap->scanline(row)[column++] = streamer.read_u32();
                } else {
                    context.bitmap->scanline(row)[column++] = int_to_scaled_rgb(context, streamer.read_u32());
                }
                break;
            }
        }

        auto consumed = space_remaining_before_consuming_row - streamer.remaining();

        return process_row_padding(consumed);
    };

    auto process_mask_row = [&](u32 row) -> ErrorOr<void> {
        u32 space_remaining_before_consuming_row = streamer.remaining();

        for (u32 column = 0; column < width;) {
            if (!streamer.has_u8())
                return Error::from_string_literal("Cannot read 8 bits");

            u8 byte = streamer.read_u8();
            u8 mask = 8;
            while (column < width && mask > 0) {
                mask -= 1;
                // apply transparency mask
                // AND mask = 0 -> fully opaque
                // AND mask = 1 -> fully transparent
                u8 and_byte = (byte >> (mask)) & 0x1;
                auto pixel = context.bitmap->scanline(row)[column];

                if (and_byte) {
                    pixel &= 0x00ffffff;
                } else if (context.dib.core.bpp < 32) {
                    pixel |= 0xff000000;
                }

                context.bitmap->scanline(row)[column++] = pixel;
            }
        }

        auto consumed = space_remaining_before_consuming_row - streamer.remaining();
        return process_row_padding(consumed);
    };

    if (context.dib.core.height < 0) {
        // BMP is stored top-down
        for (u32 row = 0; row < height; ++row) {
            TRY(process_row(row));
        }

        if (context.is_included_in_ico && !streamer.at_end()) {
            for (u32 row = 0; row < height; ++row) {
                TRY(process_mask_row(row));
            }
        }
    } else {
        // BMP is stored bottom-up
        for (i32 row = height - 1; row >= 0; --row) {
            TRY(process_row(row));
        }

        if (context.is_included_in_ico && !streamer.at_end()) {
            for (i32 row = height - 1; row >= 0; --row) {
                TRY(process_mask_row(row));
            }
        }
    }

    context.state = BMPLoadingContext::State::PixelDataDecoded;

    return {};
}

BMPImageDecoderPlugin::BMPImageDecoderPlugin(u8 const* data, size_t data_size, IncludedInICO is_included_in_ico)
{
    m_context = make<BMPLoadingContext>();
    m_context->file_bytes = data;
    m_context->file_size = data_size;
    m_context->is_included_in_ico = (is_included_in_ico == IncludedInICO::Yes);
}

BMPImageDecoderPlugin::~BMPImageDecoderPlugin() = default;

IntSize BMPImageDecoderPlugin::size()
{
    return { m_context->dib.core.width, abs(m_context->dib.core.height) };
}

bool BMPImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    BMPLoadingContext context;
    context.file_bytes = data.data();
    context.file_size = data.size();
    return !decode_bmp_header(context).is_error();
}

ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> BMPImageDecoderPlugin::create_impl(ReadonlyBytes data, IncludedInICO included_in_ico)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) BMPImageDecoderPlugin(data.data(), data.size(), included_in_ico)));
    TRY(decode_bmp_dib(*plugin->m_context));
    return plugin;
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> BMPImageDecoderPlugin::create(ReadonlyBytes data)
{
    return create_impl(data, IncludedInICO::No);
}

ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> BMPImageDecoderPlugin::create_as_included_in_ico(Badge<ICOImageDecoderPlugin>, ReadonlyBytes data)
{
    return create_impl(data, IncludedInICO::Yes);
}

bool BMPImageDecoderPlugin::sniff_dib()
{
    return !decode_bmp_dib(*m_context).is_error();
}

ErrorOr<ImageFrameDescriptor> BMPImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("BMPImageDecoderPlugin: Invalid frame index");

    if (m_context->state == BMPLoadingContext::State::Error)
        return Error::from_string_literal("BMPImageDecoderPlugin: Decoding failed");

    if (m_context->state < BMPLoadingContext::State::PixelDataDecoded)
        TRY(decode_bmp_pixel_data(*m_context));

    VERIFY(m_context->bitmap);
    return ImageFrameDescriptor { m_context->bitmap, 0 };
}

ErrorOr<Optional<ReadonlyBytes>> BMPImageDecoderPlugin::icc_data()
{
    if (m_context->dib_type != DIBType::V5)
        return OptionalNone {};

    // FIXME: For LINKED, return data from the linked file?
    // FIXME: For sRGB and WINDOWS_COLOR_SPACE, return an sRGB profile somehow.
    // FIXME: For CALIBRATED_RGB, do something with v4.{red_endpoint,green_endpoint,blue_endpoint,gamma_endpoint}
    if (m_context->dib.v4.color_space != ColorSpace::EMBEDDED)
        return OptionalNone {};

    auto const& v5 = m_context->dib.v5;
    if (!v5.profile_data || !v5.profile_size)
        return OptionalNone {};

    // FIXME: Do something with v5.intent (which has a GamutMappingIntent value).

    u8 header_size = m_context->is_included_in_ico ? 0 : bmp_header_size;
    if (v5.profile_data + header_size + v5.profile_size > m_context->file_size)
        return Error::from_string_literal("BMPImageDecoderPlugin: ICC profile data out of bounds");

    return ReadonlyBytes { m_context->file_bytes + header_size + v5.profile_data, v5.profile_size };
}

}
