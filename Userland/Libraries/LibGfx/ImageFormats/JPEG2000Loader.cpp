/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Enumerate.h>
#include <AK/MemoryStream.h>
#include <LibGfx/ImageFormats/ISOBMFF/JPEG2000Boxes.h>
#include <LibGfx/ImageFormats/ISOBMFF/Reader.h>
#include <LibGfx/ImageFormats/JPEG2000Loader.h>
#include <LibTextCodec/Decoder.h>

// Core coding system spec (.jp2 format): T-REC-T.800-201511-S!!PDF-E.pdf available here:
// https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-T.800-201511-S!!PDF-E&type=items

// There is a useful example bitstream in the spec in:
// J.10 An example of decoding showing intermediate

// Extensions (.jpx format): T-REC-T.801-202106-S!!PDF-E.pdf available here:
// https://handle.itu.int/11.1002/1000/14666-en?locatt=format:pdf&auth

// rfc3745 lists the MIME type. It only mentions the jp2_id_string as magic number.

namespace Gfx {

// A JPEG2000 image can be stored in a codestream with markers, similar to a JPEG image,
// or in a JP2 file, which is a container format based on boxes similar to ISOBMFF.

// This is the marker for the codestream version. We don't support this yet.
// T.800 Annex A, Codestream syntax, A.2 Information in the marker segments and A.3 Construction of the codestream
static constexpr u8 marker_id_string[] = { 0xFF, 0x4F, 0xFF, 0x51 };

// This is the marker for the box version.
// T.800 Annex I, JP2 file format syntax, I.5.1 JPEG 2000 Signature box
static constexpr u8 jp2_id_string[] = { 0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A };

// Table A.2 – List of markers and marker segments
// "Delimiting markers and marker segments"
#define J2K_SOC 0xFF4F // "Start of codestream"
#define J2K_SOT 0xFF90 // "Start of tile-part"
#define J2K_SOD 0xFF93 // "Start of data"
#define J2K_EOC 0xFFD9 // "End of codestream"
// "Fixed information marker segments"
#define J2K_SIZ 0xFF51 // "Image and tile size"
// "Functional marker segments"
#define J2K_COD 0xFF52 // "Coding style default"
#define J2K_COC 0xFF53 // "Coding style component"
#define J2K_RGN 0xFF5E // "Region-of-interest"
#define J2K_QCD 0xFF5C // "Quantization default"
#define J2K_QCC 0xFF5D // "Quantization component"
#define J2K_POC 0xFF5F // "Progression order change"
// "Pointer marker segments"
#define J2K_TLM 0xFF55 // "Tile-part lengths"
#define J2K_PLM 0xFF57 // "Packet length, main header"
#define J2K_PLT 0xFF58 // "Packet length, tile-part header"
#define J2K_PPM 0xFF60 // "Packed packet headers, main header"
#define J2K_PPT 0xFF61 // "Packed packet headers, tile-part header"
// "In-bit-stream markers and marker segments"
#define J2K_SOP 0xFF91 // "Start of packet"
#define J2K_EPH 0xFF92 // "End of packet header"
// "Informational marker segments"
#define J2K_CRG 0xFF63 // "Component registration"
#define J2K_COM 0xFF64 // "Comment"

// A.4.2 Start of tile-part (SOT)
struct StartOfTilePart {
    // "Tile index. This number refers to the tiles in raster order starting at the number 0."
    u16 tile_index { 0 }; // "Isot" in spec.

    // "Length, in bytes, from the beginning of the first byte of this SOT marker segment of the tile-part to
    //  the end of the data of that tile-part. Figure A.16 shows this alignment. Only the last tile-part in the
    //  codestream may contain a 0 for Psot. If the Psot is 0, this tile-part is assumed to contain all data until
    //  the EOC marker."
    u32 tile_part_length { 0 }; // "Psot" in spec.

    // "Tile-part index. There is a specific order required for decoding tile-parts; this index denotes the order
    //  from 0. If there is only one tile-part for a tile, then this value is zero. The tile-parts of this tile shall
    //  appear in the codestream in this order, although not necessarily consecutively."
    u8 tile_part_index { 0 }; // "TPsot" in spec.

    // "Number of tile-parts of a tile in the codestream. Two values are allowed: the correct number of tile-
    //  parts for that tile and zero. A zero value indicates that the number of tile-parts of this tile is not
    //  specified in this tile-part.
    u8 number_of_tile_parts { 0 }; // "TNsot" in spec.
};

static ErrorOr<StartOfTilePart> read_start_of_tile_part(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };

    StartOfTilePart sot;
    sot.tile_index = TRY(stream.read_value<BigEndian<u16>>());
    sot.tile_part_length = TRY(stream.read_value<BigEndian<u32>>());
    sot.tile_part_index = TRY(stream.read_value<u8>());
    sot.number_of_tile_parts = TRY(stream.read_value<u8>());

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: SOT marker segment: tile_index={}, tile_part_length={}, tile_part_index={}, number_of_tile_parts={}", sot.tile_index, sot.tile_part_length, sot.tile_part_index, sot.number_of_tile_parts);

    return sot;
}

// A.5.1 Image and tile size (SIZ)
struct ImageAndTileSize {
    // "Denotes capabilities that a decoder needs to properly decode the codestream."
    u16 needed_decoder_capabilities { 0 }; // "Rsiz" in spec.

    // "Width of the reference grid."
    u32 width { 0 }; // "Xsiz" in spec.

    // "Height of the reference grid."
    u32 height { 0 }; // "Ysiz" in spec.

    // "Horizontal offset from the origin of the reference grid to the left side of the image area."
    u32 x_offset { 0 }; // "XOsiz" in spec.

    // "Vertical offset from the origin of the reference grid to the top side of the image area."
    u32 y_offset { 0 }; // "YOsiz" in spec.

    // "Width of one reference tile with respect to the reference grid."
    u32 tile_width { 0 }; // "XTsiz" in spec.

    // "Height of one reference tile with respect to the reference grid."
    u32 tile_height { 0 }; // "YTsiz" in spec.

    // "Horizontal offset from the origin of the reference grid to the left side of the first tile."
    u32 tile_x_offset { 0 }; // "XTOsiz" in spec.

    // "Vertical offset from the origin of the reference grid to the top side of the first tile."
    u32 tile_y_offset { 0 }; // "YTOsiz" in spec.

    // "Csiz" isn't stored in this struct. It corresponds to `components.size()`.

    struct ComponentInformation {
        // "Precision (depth) in bits and sign of the ith component samples."
        u8 depth_and_sign { 0 }; // "Ssiz" in spec.

        // Table A.11 – Component Ssiz parameter
        u8 bit_depth() const { return (depth_and_sign & 0x7F) + 1; }
        bool is_signed() const { return depth_and_sign & 0x80; }

        // "Horizontal separation of a sample of the ith component with respect to the reference grid."
        u8 horizontal_separation { 0 }; // "XRsiz" in spec.

        // "Vertical separation of a sample of the ith component with respect to the reference grid."
        u8 vertical_separation { 0 }; // "YRsiz" in spec.
    };
    Vector<ComponentInformation> components;
};

static ErrorOr<ImageAndTileSize> read_image_and_tile_size(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };

    ImageAndTileSize siz;
    siz.needed_decoder_capabilities = TRY(stream.read_value<BigEndian<u16>>());
    siz.width = TRY(stream.read_value<BigEndian<u32>>());
    siz.height = TRY(stream.read_value<BigEndian<u32>>());
    siz.x_offset = TRY(stream.read_value<BigEndian<u32>>());
    siz.y_offset = TRY(stream.read_value<BigEndian<u32>>());
    siz.tile_width = TRY(stream.read_value<BigEndian<u32>>());
    siz.tile_height = TRY(stream.read_value<BigEndian<u32>>());
    siz.tile_x_offset = TRY(stream.read_value<BigEndian<u32>>());
    siz.tile_y_offset = TRY(stream.read_value<BigEndian<u32>>());
    u16 component_count = TRY(stream.read_value<BigEndian<u16>>()); // "Csiz" in spec.

    for (size_t i = 0; i < component_count; ++i) {
        ImageAndTileSize::ComponentInformation component;
        component.depth_and_sign = TRY(stream.read_value<u8>());
        if (component.bit_depth() > 38)
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid component depth");
        component.horizontal_separation = TRY(stream.read_value<u8>());
        component.vertical_separation = TRY(stream.read_value<u8>());
        siz.components.append(component);
    }

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: SIZ marker segment: needed_decoder_capabilities={}, width={}, height={}, x_offset={}, y_offset={}, tile_width={}, tile_height={}, tile_x_offset={}, tile_y_offset={}", siz.needed_decoder_capabilities, siz.width, siz.height, siz.x_offset, siz.y_offset, siz.tile_width, siz.tile_height, siz.tile_x_offset, siz.tile_y_offset);
    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: SIZ marker segment: {} components:", component_count);
    for (auto [i, component] : enumerate(siz.components))
        dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: SIZ marker segment: component[{}]: is_signed={}, bit_depth={}, horizontal_separation={}, vertical_separation={}", i, component.is_signed(), component.bit_depth(), component.horizontal_separation, component.vertical_separation);

    return siz;
}

// Data shared by COD and COC marker segments
struct CodingStyleParameters {
    // Table A.20 – Transformation for the SPcod and SPcoc parameters
    enum Transformation {
        Irreversible_9_7_Filter = 0,
        Reversible_5_3_Filter = 1,
    };

    // Table A.15 – Coding style parameter values of the SPcod and SPcoc parameters
    // "Number of decomposition levels, NL, Zero implies no transformation."
    u8 number_of_decomposition_levels { 0 };
    u8 code_block_width_exponent { 0 };  // "xcb" in spec; 2 already added.
    u8 code_block_height_exponent { 0 }; // "ycb" in spec; 2 already added.
    u8 code_block_style { 0 };
    Transformation transformation { Irreversible_9_7_Filter };

    // Table A.19 – Code-block style for the SPcod and SPcoc parameters
    bool uses_selective_arithmetic_coding_bypass() const { return code_block_style & 1; }
    bool reset_context_probabilities() const { return code_block_style & 2; }
    bool uses_termination_on_each_coding_pass() const { return code_block_style & 4; }
    bool uses_vertically_causal_context() const { return code_block_style & 8; }
    bool uses_predictable_termination() const { return code_block_style & 0x10; }
    bool uses_segmentation_symbols() const { return code_block_style & 0x20; }

    // If has_explicit_precinct_size is false, this contains the default { 15, 15 } number_of_decomposition_levels + 1 times.
    // If has_explicit_precinct_size is true, this contains number_of_decomposition_levels + 1 explicit values stored in the COD marker segment.
    struct PrecinctSize {
        u8 PPx { 0 };
        u8 PPy { 0 };
    };
    Vector<PrecinctSize> precinct_sizes;
};

static ErrorOr<CodingStyleParameters> read_coding_style_parameters(ReadonlyBytes data, StringView name, bool has_explicit_precinct_size)
{
    FixedMemoryStream stream { data };

    CodingStyleParameters parameters;

    parameters.number_of_decomposition_levels = TRY(stream.read_value<u8>());
    if (parameters.number_of_decomposition_levels > 32)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid number of decomposition levels");

    // Table A.18 – Width or height exponent of the code-blocks for the SPcod and SPcoc parameters
    u8 xcb = (TRY(stream.read_value<u8>()) & 0xF) + 2;
    u8 ycb = (TRY(stream.read_value<u8>()) & 0xF) + 2;
    if (xcb > 10 || ycb > 10 || xcb + ycb > 12)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid code block size");
    parameters.code_block_width_exponent = xcb;
    parameters.code_block_height_exponent = ycb;

    parameters.code_block_style = TRY(stream.read_value<u8>());

    u8 transformation = TRY(stream.read_value<u8>());
    if (transformation > 1)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid transformation");
    parameters.transformation = static_cast<CodingStyleParameters::Transformation>(transformation);

    if (has_explicit_precinct_size) {
        for (size_t i = 0; i < parameters.number_of_decomposition_levels + 1u; ++i) {
            u8 b = TRY(stream.read_value<u8>());

            // Table A.21 – Precinct width and height for the SPcod and SPcoc parameters
            CodingStyleParameters::PrecinctSize precinct_size;
            precinct_size.PPx = b & 0xF;
            precinct_size.PPy = b >> 4;
            if ((precinct_size.PPx == 0 || precinct_size.PPy == 0) && i > 0)
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid precinct size");
            parameters.precinct_sizes.append(precinct_size);
        }
    } else {
        for (size_t i = 0; i < parameters.number_of_decomposition_levels + 1u; ++i)
            parameters.precinct_sizes.append({ 15, 15 });
    }

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: number_of_decomposition_levels={}, code_block_width_exponent={}, code_block_height_exponent={}", name, parameters.number_of_decomposition_levels, parameters.code_block_width_exponent, parameters.code_block_height_exponent);
    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: code_block_style={}, transformation={}", name, parameters.code_block_style, (int)parameters.transformation);
    if (has_explicit_precinct_size) {
        dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: {} explicit precinct sizes:", name, parameters.precinct_sizes.size());
        for (auto [i, precinct_size] : enumerate(parameters.precinct_sizes))
            dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: precinct_size[{}]: PPx={}, PPy={}", name, i, precinct_size.PPx, precinct_size.PPy);
    }

    return parameters;
}

// A.6.1 Coding style default (COD)
struct CodingStyleDefault {
    // Table A.13 – Coding style parameter values for the Scod parameter
    bool has_explicit_precinct_size { false };
    bool may_use_SOP_marker { false };
    bool may_use_EPH_marker { false };

    // Table A.16 – Progression order for the SGcod, SPcoc, and Ppoc parameters
    // B.12 Progression order
    enum ProgressionOrder {
        LayerResolutionComponentPosition = 0,
        ResolutionLayerComponentPosition = 1,
        ResolutionPositionComponentLayer = 2,
        PositionComponentResolutionLayer = 3,
        ComponentPositionResolutionLayer = 4,
    };

    // Table A.17 – Multiple component transformation for the SGcod parameters
    enum MultipleComponentTransformationType {
        None = 0,
        MultipleComponentTransformationUsed = 1, // See Annex G
    };

    // Table A.14 – Coding style parameter values of the SGcod parameter
    ProgressionOrder progression_order { LayerResolutionComponentPosition };
    u16 number_of_layers { 0 };
    MultipleComponentTransformationType multiple_component_transformation_type { None };

    CodingStyleParameters parameters;
};

static ErrorOr<CodingStyleDefault> read_coding_style_default(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };

    CodingStyleDefault cod;

    u8 Scod = TRY(stream.read_value<u8>());
    cod.has_explicit_precinct_size = Scod & 1;
    cod.may_use_SOP_marker = Scod & 2;
    cod.may_use_EPH_marker = Scod & 4;

    u32 SGcod = TRY(stream.read_value<BigEndian<u32>>());
    u8 progression_order = SGcod >> 24;
    if (progression_order > 4)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid progression order");
    cod.progression_order = static_cast<CodingStyleDefault::ProgressionOrder>(progression_order);

    cod.number_of_layers = (SGcod >> 8) & 0xFFFF;
    if (cod.number_of_layers == 0)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid number of layers");

    u8 multiple_component_transformation_type = SGcod & 0xFF;
    if (multiple_component_transformation_type > 1)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid multiple component transformation type");
    cod.multiple_component_transformation_type = static_cast<CodingStyleDefault::MultipleComponentTransformationType>(multiple_component_transformation_type);

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: COD marker segment: has_explicit_precinct_size={}, may_use_SOP_marker={}, may_use_EPH_marker={}", cod.has_explicit_precinct_size, cod.may_use_SOP_marker, cod.may_use_EPH_marker);
    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: COD marker segment: progression_order={}, number_of_layers={}, multiple_component_transformation_type={}", (int)cod.progression_order, cod.number_of_layers, (int)cod.multiple_component_transformation_type);

    cod.parameters = TRY(read_coding_style_parameters(data.slice(stream.offset()), "COD"sv, cod.has_explicit_precinct_size));

    return cod;
}

// A.6.2 Coding style component (COC)
struct CodingStyleComponent {
    u16 component_index { 0 }; // "Ccoc" in spec.

    // Table A.23 – Coding style parameter values for the Scoc parameter
    bool has_explicit_precinct_size { false }; // "Scoc" in spec.

    CodingStyleParameters parameters;
};

static ErrorOr<CodingStyleComponent> read_coding_style_component(ReadonlyBytes data, size_t number_of_components)
{
    FixedMemoryStream stream { data };

    // Table A.22 – Coding style component parameter values
    CodingStyleComponent coc;
    if (number_of_components < 257)
        coc.component_index = TRY(stream.read_value<u8>());
    else
        coc.component_index = TRY(stream.read_value<BigEndian<u16>>());

    u8 Scoc = TRY(stream.read_value<u8>());
    coc.has_explicit_precinct_size = Scoc & 1;

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: COC marker segment: component_index={}", coc.component_index);
    coc.parameters = TRY(read_coding_style_parameters(data.slice(TRY(stream.tell())), "COC"sv, coc.has_explicit_precinct_size));

    return coc;
}

// A.6.4 Quantization default (QCD)
struct QuantizationDefault {
    enum QuantizationStyle {
        NoQuantization = 0,
        ScalarDerived = 1,
        ScalarExpounded = 2,
    };
    QuantizationStyle quantization_style { NoQuantization };
    u8 number_of_guard_bits { 0 };

    struct ReversibleStepSize {
        u8 exponent { 0 };
    };
    struct IrreversibleStepSize {
        u16 mantissa { 0 };
        u8 exponent { 0 };
    };

    // Stores a Vector<ReversibleStepSize> if quantization_style is NoQuantization, and a Vector<IrreversibleStepSize> otherwise.
    // The size of the vector is >= 3*number_of_decomposition_levels + 1 if quantization_style is not ScalarDerived, and 1 otherwise.
    using StepSizeType = Variant<Empty, Vector<ReversibleStepSize>, Vector<IrreversibleStepSize>>;
    StepSizeType step_sizes;
};

static ErrorOr<QuantizationDefault> read_quantization_default(ReadonlyBytes data, StringView marker_name = "QCD"sv)
{
    FixedMemoryStream stream { data };

    QuantizationDefault qcd;

    u8 sqcd = TRY(stream.read_value<u8>());
    u8 quantization_style = sqcd & 0x1F;
    if (quantization_style > 2)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid quantization style");
    qcd.quantization_style = static_cast<QuantizationDefault::QuantizationStyle>(quantization_style);
    qcd.number_of_guard_bits = sqcd >> 5;

    qcd.step_sizes = TRY([&]() -> ErrorOr<QuantizationDefault::StepSizeType> {
        if (quantization_style == QuantizationDefault::NoQuantization) {
            // Table A.29 – Reversible step size values for the SPqcd and SPqcc parameters (reversible transform only)
            if (data.size() < 2)
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough data for QCD marker segment");
            u8 number_of_decomposition_levels = (data.size() - 2) / 3;

            Vector<QuantizationDefault::ReversibleStepSize> reversible_step_sizes;
            for (size_t i = 0; i < 1u + 3u * number_of_decomposition_levels; ++i)
                reversible_step_sizes.append({ static_cast<u8>(TRY(stream.read_value<u8>()) >> 3) });
            return reversible_step_sizes;
        }

        // Table A.30 – Quantization values for the SPqcd and SPqcc parameters (irreversible transformation only)
        if (data.size() < 3)
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough data for QCD marker segment");
        u8 number_of_decomposition_levels = 0;
        if (quantization_style == QuantizationDefault::ScalarExpounded)
            number_of_decomposition_levels = (data.size() - 3) / 6;

        Vector<QuantizationDefault::IrreversibleStepSize> irreversible_step_sizes;
        for (size_t i = 0; i < 1u + 3u * number_of_decomposition_levels; ++i) {
            u16 value = TRY(stream.read_value<BigEndian<u16>>());
            QuantizationDefault::IrreversibleStepSize step_size;
            step_size.mantissa = value & 0x7FF;
            step_size.exponent = value >> 11;
            irreversible_step_sizes.append(step_size);
        }
        return irreversible_step_sizes;
    }());

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: quantization_style={}, number_of_guard_bits={}", marker_name, (int)qcd.quantization_style, qcd.number_of_guard_bits);
    qcd.step_sizes.visit(
        [](Empty) { VERIFY_NOT_REACHED(); },
        [&](Vector<QuantizationDefault::ReversibleStepSize> const& step_sizes) {
            dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: {} step sizes:", marker_name, step_sizes.size());
            for (auto [i, step_size] : enumerate(step_sizes)) {
                dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: step_size[{}]: exponent={}", marker_name, i, step_size.exponent);
            }
        },
        [&](Vector<QuantizationDefault::IrreversibleStepSize> const& step_sizes) {
            dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: {} step sizes:", marker_name, step_sizes.size());
            for (auto [i, step_size] : enumerate(step_sizes)) {
                dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: {} marker segment: step_size[{}]: mantissa={}, exponent={}", marker_name, i, step_size.mantissa, step_size.exponent);
            }
        });

    return qcd;
}

// A.6.5 Quantization component (QCC)
struct QuantizationComponent {
    u16 component_index { 0 }; // "Cqcc" in spec.
    QuantizationDefault qcd;
};

static ErrorOr<QuantizationComponent> read_quantization_component(ReadonlyBytes data, size_t number_of_components)
{
    FixedMemoryStream stream { data };

    QuantizationComponent qcc;
    if (number_of_components < 257)
        qcc.component_index = TRY(stream.read_value<u8>());
    else
        qcc.component_index = TRY(stream.read_value<BigEndian<u16>>());

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: QCC marker segment: component_index={}", qcc.component_index);
    qcc.qcd = TRY(read_quantization_default(data.slice(TRY(stream.tell())), "QCC"sv));

    return qcc;
}

// A.9.2 Comment (COM)
struct Comment {
    enum CommentType {
        Binary = 0,
        ISO_IEC_8859_15 = 1,
    };
    CommentType type { Binary }; // "Rcom" in spec.
    ReadonlyBytes data;
};

static ErrorOr<Comment> read_comment(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };

    Comment com;
    u16 comment_type = TRY(stream.read_value<BigEndian<u16>>());
    if (comment_type > 1)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid comment type");
    com.type = static_cast<Comment::CommentType>(comment_type);
    com.data = data.slice(TRY(stream.tell()));

    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: COM marker segment: comment_type={}, size()={}", (int)com.type, com.data.size());
    if (com.type == Comment::ISO_IEC_8859_15)
        dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: COM marker segment, ISO/IEC 8859-15 text: '{}'", TRY(TextCodec::decoder_for("ISO-8859-15"sv)->to_utf8(StringView { com.data })));

    return com;
}

struct TilePartData {
    StartOfTilePart sot;
    Vector<Comment> coms;
    ReadonlyBytes data;
};

struct TileData {
    Optional<CodingStyleDefault> cod;
    Vector<CodingStyleComponent> cocs;
    Optional<QuantizationDefault> qcd;
    Vector<QuantizationComponent> qccs;
    Vector<TilePartData> tile_parts;
};

struct JPEG2000LoadingContext {
    enum class State {
        NotDecoded = 0,
        DecodedTileHeaders,
        Error,
    };
    State state { State::NotDecoded };
    ReadonlyBytes codestream_data;
    size_t codestream_cursor { 0 };
    Optional<ReadonlyBytes> icc_data;

    IntSize size;

    ISOBMFF::BoxList boxes;

    // Data from marker segments:
    ImageAndTileSize siz;
    CodingStyleDefault cod;
    Vector<CodingStyleComponent> cocs;
    QuantizationDefault qcd;
    Vector<QuantizationComponent> qccs;
    Vector<Comment> coms;
    Vector<TileData> tiles;
};

struct MarkerSegment {
    u16 marker;

    // OptionalNone for markers that don't have data.
    // For markers that do have data, this does not include the marker length data. (`data.size() + 2` is the value of the marker length field.)
    Optional<ReadonlyBytes> data;
};

static ErrorOr<u16> peek_marker(JPEG2000LoadingContext& context)
{
    if (context.codestream_cursor + 2 > context.codestream_data.size())
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough data for marker");
    return *reinterpret_cast<BigEndian<u16> const*>(context.codestream_data.data() + context.codestream_cursor);
}

static ErrorOr<MarkerSegment> read_marker_at_cursor(JPEG2000LoadingContext& context)
{
    u16 marker = TRY(peek_marker(context));
    // "All markers with the marker code between 0xFF30 and 0xFF3F have no marker segment parameters. They shall be skipped by the decoder."
    // "The SOC, SOD and EOC are delimiting markers not marker segments, and have no explicit length information or other parameters."
    bool is_marker_segment = !(marker >= 0xFF30 && marker <= 0xFF3F) && marker != J2K_SOC && marker != J2K_SOD && marker != J2K_EOC;

    MarkerSegment marker_segment;
    marker_segment.marker = marker;

    if (is_marker_segment) {
        if (context.codestream_cursor + 4 > context.codestream_data.size())
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough data for marker segment length");
        u16 marker_length = *reinterpret_cast<BigEndian<u16> const*>(context.codestream_data.data() + context.codestream_cursor + 2);
        if (marker_length < 2)
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Marker segment length too small");
        if (context.codestream_cursor + 2 + marker_length > context.codestream_data.size())
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough data for marker segment data");
        marker_segment.data = ReadonlyBytes { context.codestream_data.data() + context.codestream_cursor + 4, marker_length - 2u };
    }

    context.codestream_cursor += 2;
    if (is_marker_segment)
        context.codestream_cursor += 2 + marker_segment.data->size();

    return marker_segment;
}

static ErrorOr<void> parse_codestream_main_header(JPEG2000LoadingContext& context)
{
    // Figure A.3 – Construction of the main header
    // "Required as the first marker"
    auto marker = TRY(read_marker_at_cursor(context));
    if (marker.marker != J2K_SOC)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected SOC marker");

    // "Required as the second marker segment"
    marker = TRY(read_marker_at_cursor(context));
    if (marker.marker != J2K_SIZ)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected SIZ marker");
    context.siz = TRY(read_image_and_tile_size(marker.data.value()));

    bool saw_COD_marker = false;
    bool saw_QCD_marker = false;
    while (true) {
        u16 marker = TRY(peek_marker(context));
        switch (marker) {
        case J2K_COD:
        case J2K_COC:
        case J2K_QCD:
        case J2K_QCC:
        case J2K_RGN:
        case J2K_POC:
        case J2K_PPM:
        case J2K_TLM:
        case J2K_PLM:
        case J2K_CRG:
        case J2K_COM: {
            auto marker = TRY(read_marker_at_cursor(context));
            if (marker.marker == J2K_COD) {
                if (saw_COD_marker)
                    return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Multiple COD markers in main header");
                context.cod = TRY(read_coding_style_default(marker.data.value()));
                saw_COD_marker = true;
            } else if (marker.marker == J2K_COC) {
                context.cocs.append(TRY(read_coding_style_component(marker.data.value(), context.siz.components.size())));
            } else if (marker.marker == J2K_QCD) {
                if (saw_QCD_marker)
                    return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Multiple QCD markers in main header");
                context.qcd = TRY(read_quantization_default(marker.data.value()));
                saw_QCD_marker = true;
            } else if (marker.marker == J2K_QCC) {
                context.qccs.append(TRY(read_quantization_component(marker.data.value(), context.siz.components.size())));
            } else if (marker.marker == J2K_COM) {
                context.coms.append(TRY(read_comment(marker.data.value())));
            } else {
                // FIXME: These are valid main header markers. Parse contents.
                dbgln("JPEG2000ImageDecoderPlugin: marker {:#04x} not yet implemented", marker.marker);
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: marker not yet implemented");
            }
            break;
        }
        case J2K_SOT: {
            // SOT terminates the main header.
            // A.4.2: "There shall be at least one SOT in a codestream."
            if (!saw_COD_marker)
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Required COD marker not present in main header");
            if (!saw_QCD_marker)
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Required QCD marker not present in main header");

            // A.6.4: "there is not necessarily a correspondence with the number of sub-bands present because the sub-bands
            //         can be truncated with no requirement to correct [the QCD] marker segment."
            size_t step_sizes_count = context.qcd.step_sizes.visit(
                [](Empty) -> size_t { VERIFY_NOT_REACHED(); },
                [](Vector<QuantizationDefault::ReversibleStepSize> const& step_sizes) { return step_sizes.size(); },
                [](Vector<QuantizationDefault::IrreversibleStepSize> const& step_sizes) { return step_sizes.size(); });
            // FIXME: What if number_of_decomposition_levels is in context.cocs and varies by component?
            if (context.qcd.quantization_style != QuantizationDefault::ScalarDerived && step_sizes_count < context.cod.parameters.number_of_decomposition_levels * 3u + 1u)
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough step sizes for number of decomposition levels");

            return {};
        }
        default:
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Unexpected marker in main header");
        }
    }
}

static ErrorOr<void> parse_codestream_tile_header(JPEG2000LoadingContext& context)
{
    // Figure A.4 – Construction of the first tile-part header of a given tile
    // Figure A.5 – Construction of a non-first tile-part header

    // "Required as the first marker segment of every tile-part header"
    auto tile_start = context.codestream_cursor;
    auto marker = TRY(read_marker_at_cursor(context));
    if (marker.marker != J2K_SOT)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected SOT marker");
    auto start_of_tile = TRY(read_start_of_tile_part(marker.data.value()));
    // FIXME: Store start_of_tile on context somewhere.

    context.tiles.resize(max(context.tiles.size(), (size_t)start_of_tile.tile_index + 1));
    auto& tile = context.tiles[start_of_tile.tile_index];

    if (tile.tile_parts.size() != start_of_tile.tile_part_index)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Tile part index out of order");
    tile.tile_parts.append({});
    auto& tile_part = tile.tile_parts.last();
    tile_part.sot = start_of_tile;

    bool found_start_of_data = false;
    while (!found_start_of_data) {
        u16 marker = TRY(peek_marker(context));
        switch (marker) {
        case J2K_SOD:
            // "Required as the last marker segment of every tile-part header"
            context.codestream_cursor += 2;
            found_start_of_data = true;
            break;
        case J2K_COD:
        case J2K_COC:
        case J2K_QCD:
        case J2K_QCC:
        case J2K_RGN:
            if (start_of_tile.tile_part_index != 0)
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: COD, COC, QCD, QCC, RGN markers are only valid in the first tile-part header");
            [[fallthrough]];
        case J2K_POC:
        case J2K_PPT:
        case J2K_PLT:
        case J2K_COM: {
            auto marker = TRY(read_marker_at_cursor(context));
            if (marker.marker == J2K_COD) {
                if (tile.cod.has_value())
                    return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Multiple COD markers in tile header");
                tile.cod = TRY(read_coding_style_default(marker.data.value()));
            } else if (marker.marker == J2K_COC) {
                tile.cocs.append(TRY(read_coding_style_component(marker.data.value(), context.siz.components.size())));
            } else if (marker.marker == J2K_QCD) {
                if (tile.qcd.has_value())
                    return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Multiple QCD markers in tile header");
                tile.qcd = TRY(read_quantization_default(marker.data.value()));
            } else if (marker.marker == J2K_QCC) {
                tile.qccs.append(TRY(read_quantization_component(marker.data.value(), context.siz.components.size())));
            } else if (marker.marker == J2K_COM) {
                tile_part.coms.append(TRY(read_comment(marker.data.value())));
            } else {
                // FIXME: These are valid main header markers. Parse contents.
                dbgln("JPEG2000ImageDecoderPlugin: marker {:#04x} not yet implemented in tile header", marker.marker);
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: marker not yet implemented in tile header");
            }
            break;
        }
        default:
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Unexpected marker in tile header");
        }
    }

    u32 tile_bitstream_length;
    if (start_of_tile.tile_part_length == 0) {
        // Leave room for EOC marker.
        if (context.codestream_data.size() - context.codestream_cursor < 2)
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough data for EOC marker");
        tile_bitstream_length = context.codestream_data.size() - context.codestream_cursor - 2;
    } else {
        u32 tile_header_length = context.codestream_cursor - tile_start;
        if (start_of_tile.tile_part_length < tile_header_length)
            return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid tile part length");
        tile_bitstream_length = start_of_tile.tile_part_length - tile_header_length;
    }

    if (context.codestream_cursor + tile_bitstream_length > context.codestream_data.size())
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Not enough data for tile bitstream");
    tile_part.data = context.codestream_data.slice(context.codestream_cursor, tile_bitstream_length);

    context.codestream_cursor += tile_bitstream_length;
    dbgln_if(JPEG2000_DEBUG, "JPEG2000ImageDecoderPlugin: Tile bitstream length: {}", tile_bitstream_length);

    return {};
}

static ErrorOr<void> parse_codestream_tile_headers(JPEG2000LoadingContext& context)
{
    while (true) {
        auto marker = TRY(peek_marker(context));
        if (marker == J2K_EOC) {
            context.codestream_cursor += 2;
            break;
        }
        TRY(parse_codestream_tile_header(context));
    }

    if (context.codestream_cursor < context.codestream_data.size())
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Unexpected data after EOC marker");
    return {};
}

static ErrorOr<void> decode_jpeg2000_header(JPEG2000LoadingContext& context, ReadonlyBytes data)
{
    if (!JPEG2000ImageDecoderPlugin::sniff(data))
        return Error::from_string_literal("JPEG2000LoadingContext: Invalid JPEG2000 header");

    if (data.starts_with(marker_id_string)) {
        context.codestream_data = data;
        TRY(parse_codestream_main_header(context));
        context.size = { context.siz.width, context.siz.height };
        return {};
    }

    auto reader = TRY(Gfx::ISOBMFF::Reader::create(TRY(try_make<FixedMemoryStream>(data))));
    context.boxes = TRY(reader.read_entire_file());

    // I.2.2 File organization
    // "A particular order of those boxes in the file is not generally implied. However, the JPEG 2000 Signature box
    //  shall be the first box in a JP2 file, the File Type box shall immediately follow the JPEG 2000 Signature box
    //  and the JP2 Header box shall fall before the Contiguous Codestream box."
    if (context.boxes.size() < 4)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected at least four boxes");

    // Required toplevel boxes: signature box, file type box, jp2 header box, contiguous codestream box.

    if (context.boxes[0]->box_type() != ISOBMFF::BoxType::JPEG2000SignatureBox)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected JPEG2000SignatureBox as first box");
    if (context.boxes[1]->box_type() != ISOBMFF::BoxType::FileTypeBox)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected FileTypeBox as second box");

    Optional<size_t> jp2_header_box_index;
    Optional<size_t> contiguous_codestream_box_index;
    for (size_t i = 2; i < context.boxes.size(); ++i) {
        if (context.boxes[i]->box_type() == ISOBMFF::BoxType::JPEG2000HeaderBox) {
            // "Within a JP2 file, there shall be one and only one JP2 Header box."
            if (jp2_header_box_index.has_value())
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Multiple JP2 Header boxes");
            jp2_header_box_index = i;
        }
        if (context.boxes[i]->box_type() == ISOBMFF::BoxType::JPEG2000ContiguousCodestreamBox && !contiguous_codestream_box_index.has_value()) {
            // "a conforming reader shall ignore all codestreams after the first codestream found in the file.
            //  Contiguous Codestream boxes may be found anywhere in the file except before the JP2 Header box."
            contiguous_codestream_box_index = i;
            if (!jp2_header_box_index.has_value() || contiguous_codestream_box_index.value() < jp2_header_box_index.value())
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: JP2 Header box must come before Contiguous Codestream box");
        }
    }

    if (!jp2_header_box_index.has_value())
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected JP2 Header box");
    if (!contiguous_codestream_box_index.has_value())
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected Contiguous Codestream box");

    // FIXME: JPEG2000ContiguousCodestreamBox makes a copy of the codestream data. That's too heavy for header scanning.
    // Add a mode to ISOBMFF::Reader where it only stores offsets for the codestream data and the ICC profile.
    auto const& codestream_box = static_cast<ISOBMFF::JPEG2000ContiguousCodestreamBox const&>(*context.boxes[contiguous_codestream_box_index.value()]);
    context.codestream_data = codestream_box.codestream.bytes();

    // Required child boxes of the jp2 header box: image header box, color box.

    Optional<size_t> image_header_box_index;
    Optional<size_t> color_header_box_index;
    auto const& header_box = static_cast<ISOBMFF::JPEG2000HeaderBox const&>(*context.boxes[jp2_header_box_index.value()]);
    for (size_t i = 0; i < header_box.child_boxes().size(); ++i) {
        auto const& subbox = header_box.child_boxes()[i];
        if (subbox->box_type() == ISOBMFF::BoxType::JPEG2000ImageHeaderBox) {
            if (image_header_box_index.has_value())
                return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Multiple Image Header boxes");
            image_header_box_index = i;
        }
        if (subbox->box_type() == ISOBMFF::BoxType::JPEG2000ColorSpecificationBox) {
            // T.800 says there should be just one 'colr' box, but T.801 allows several and says to pick the one with highest precedence.
            bool use_this_color_box;
            if (!color_header_box_index.has_value()) {
                use_this_color_box = true;
            } else {
                auto const& new_header_box = static_cast<ISOBMFF::JPEG2000ColorSpecificationBox const&>(*header_box.child_boxes()[i]);
                auto const& current_color_box = static_cast<ISOBMFF::JPEG2000ColorSpecificationBox const&>(*header_box.child_boxes()[color_header_box_index.value()]);
                use_this_color_box = new_header_box.precedence > current_color_box.precedence;
            }

            if (use_this_color_box)
                color_header_box_index = i;
        }
    }

    if (!image_header_box_index.has_value())
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected Image Header box");
    if (!color_header_box_index.has_value())
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Expected Color Specification box");

    auto const& image_header_box = static_cast<ISOBMFF::JPEG2000ImageHeaderBox const&>(*header_box.child_boxes()[image_header_box_index.value()]);
    context.size = { image_header_box.width, image_header_box.height };

    auto const& color_header_box = static_cast<ISOBMFF::JPEG2000ColorSpecificationBox const&>(*header_box.child_boxes()[color_header_box_index.value()]);
    if (color_header_box.method == 2 || color_header_box.method == 3)
        context.icc_data = color_header_box.icc_data.bytes();

    TRY(parse_codestream_main_header(context));

    auto size_from_siz = IntSize { context.siz.width, context.siz.height };
    if (size_from_siz != context.size) {
        // FIXME: If this is common, warn and use size from SIZ marker.
        dbgln("JPEG2000ImageDecoderPlugin: Image size from SIZ marker ({}) does not match image size from JP2 header ({})", size_from_siz, context.size);
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Image size from SIZ marker does not match image size from JP2 header");
    }

    return {};
}

namespace JPEG2000 {

// Tag trees are used to store the code-block inclusion bits and the zero bit-plane information.
// B.10.2 Tag trees
// "At every node of this tree the minimum integer of the (up to four) nodes below it is recorded. [...]
//  Level 0 is the lowest level of the tag tree; it contains the top node. [...]
//  Each node has a [...] current value, [...] initialized to zero. A 0 bit in the tag tree means that the minimum
//  (or the value in the case of the highest level) is larger than the current value and a 1 bit means that the minimum
//  (or the value in the case of the highest level) is equal to the current value.
//  For each contiguous 0 bit in the tag tree the current value is incremented by one.
//  Nodes at higher levels cannot be coded until lower level node values are fixed (i.e, a 1 bit is coded). [...]
//  Only the information needed for the current code-block is stored at the current point in the packet header."
// The example in Figure B.13 / Table B.5 is useful to understand what exactly "only the information needed" means.
struct TagTreeNode {
    u32 value { 0 };
    enum State {
        Pending,
        Final,
    };
    State state { Pending };
    Array<OwnPtr<TagTreeNode>, 4> children {};
    u32 level { 0 }; // 0 for leaf nodes, 1 for the next level, etc.

    bool is_leaf() const { return level == 0; }

    ErrorOr<u32> read_value(u32 x, u32 y, Function<ErrorOr<bool>()> const& read_bit, u32 start_value, Optional<u32> stop_at = {})
    {
        value = max(value, start_value);
        while (true) {
            if (stop_at.has_value() && value == stop_at.value())
                return value;

            if (state == Final) {
                if (is_leaf())
                    return value;
                u32 x_index = (x >> (level - 1)) & 1;
                u32 y_index = (y >> (level - 1)) & 1;
                return children[y_index * 2 + x_index]->read_value(x, y, read_bit, value, stop_at);
            }

            bool bit = TRY(read_bit());
            if (!bit)
                value++;
            else
                state = Final;
        }
    }

    static ErrorOr<NonnullOwnPtr<TagTreeNode>> create(u32 x_count, u32 y_count, u32 level)
    {
        VERIFY(x_count > 0);
        VERIFY(y_count > 0);

        auto node = TRY(try_make<TagTreeNode>());
        node->level = level;
        if (node->is_leaf()) {
            VERIFY(x_count == 1);
            VERIFY(y_count == 1);
            return node;
        }

        u32 top_left_x_child_count = min(x_count, 1u << (max(level, 1) - 1));
        u32 top_left_y_child_count = min(y_count, 1u << (max(level, 1) - 1));
        for (u32 y = 0; y < 2; ++y) {
            for (u32 x = 0; x < 2; ++x) {
                u32 child_x_count = x == 1 ? x_count - top_left_x_child_count : top_left_x_child_count;
                u32 child_y_count = y == 1 ? y_count - top_left_y_child_count : top_left_y_child_count;
                if (child_x_count == 0 || child_y_count == 0)
                    continue;
                node->children[y * 2 + x] = TRY(create(child_x_count, child_y_count, level - 1));
            }
        }
        return node;
    }
};

TagTree::TagTree(NonnullOwnPtr<TagTreeNode> root)
    : m_root(move(root))
{
}

TagTree::TagTree(TagTree&&) = default;
TagTree::~TagTree() = default;

ErrorOr<TagTree> TagTree::create(u32 x_count, u32 y_count)
{
    auto level = ceil(log2(max(x_count, y_count)));
    return TagTree { TRY(TagTreeNode::create(x_count, y_count, level)) };
}

ErrorOr<u32> TagTree::read_value(u32 x, u32 y, Function<ErrorOr<bool>()> const& read_bit, Optional<u32> stop_at) const
{
    return m_root->read_value(x, y, read_bit, m_root->value, stop_at);
}

}

bool JPEG2000ImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return data.starts_with(jp2_id_string) || data.starts_with(marker_id_string);
}

JPEG2000ImageDecoderPlugin::JPEG2000ImageDecoderPlugin()
{
    m_context = make<JPEG2000LoadingContext>();
}

JPEG2000ImageDecoderPlugin::~JPEG2000ImageDecoderPlugin() = default;

IntSize JPEG2000ImageDecoderPlugin::size()
{
    return m_context->size;
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JPEG2000ImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JPEG2000ImageDecoderPlugin()));
    TRY(decode_jpeg2000_header(*plugin->m_context, data));
    return plugin;
}

ErrorOr<ImageFrameDescriptor> JPEG2000ImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index != 0)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Invalid frame index");

    if (m_context->state == JPEG2000LoadingContext::State::Error)
        return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Decoding failed");

    if (m_context->state < JPEG2000LoadingContext::State::DecodedTileHeaders) {
        TRY(parse_codestream_tile_headers(*m_context));
        m_context->state = JPEG2000LoadingContext::State::DecodedTileHeaders;
    }

    return Error::from_string_literal("JPEG2000ImageDecoderPlugin: Draw the rest of the owl");
}

ErrorOr<Optional<ReadonlyBytes>> JPEG2000ImageDecoderPlugin::icc_data()
{
    return m_context->icc_data;
}

}
