/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/MemoryStream.h>
#include <AK/Vector.h>
#include <LibGfx/ImageFormats/BooleanDecoder.h>
#include <LibGfx/ImageFormats/WebPLoaderLossy.h>
#include <LibGfx/ImageFormats/WebPLoaderLossyTables.h>

// Lossy format: https://datatracker.ietf.org/doc/html/rfc6386

// Summary:
// A lossy webp image is a VP8 keyframe.
// A VP8 keyframe consists of 16x16 pixel tiles called macroblocks. Each macroblock is subdivided into 4x4 pixel tiles called subblocks.
// Pixel values are stored as YUV 4:2:0. That is, each 4x4 luma pixels are covered by 1 pixel U chroma and 1 pixel V chroma.
// This means one macroblock is covered by 4x4 Y subblocks and 2x2 U and V subblocks each.
// VP8 data consists of:
// * A tiny bit of uncompressed data, storing image dimensions and the size of the first compressed chunk of data, called the first partition
// * The first partition, which is a entropy-coded bitstream storing:
//   1. A fixed-size header.
//      The main piece of data this stores is a probability distribution for how pixel values of each macroblock are predicted from previously decoded data.
//      It also stores how may independent entropy-coded bitstreams are used to store the actual pixel data (for all images I've seen so far, just one).
//   2. For each macroblock, it stores how that macroblock's pixel values are predicted from previously decoded data (and some more per-macroblock metadata).
//      There are independent prediction modes for Y, U, V.
//      U and V store a single prediction mode per macroblock.
//      Y can store a single prediction mode per macroblock, or it can store one subblock prediction mode for each of the 4x4 luma subblocks.
// * One or more additional entropy-coded bitstreams ("partitions") that store the discrete cosine transform ("DCT") coefficients for the actual pixel data for each macroblock.
//   Each macroblock is subdivided into 4x4 tiles called "subblocks". A 16x16 pixel macroblock consists of:
//   0. If the macroblock stores 4x4 luma subblock prediction modes, the 4x4 DC coefficients of each subblock's DCT are stored at the start of the macroblock's data,
//      as coefficients of an inverse Walsh-Hadamard Transform (WHT).
//   1. 4x4 luma subblocks
//   2. 2x2 U chrome subblocks
//   3. 2x2 U chrome subblocks
//   That is, each macroblock stores 24 or 25 sets of coefficients.
//   Each set of coefficients stores 16 numbers, using a combination of a custom prefix tree and dequantization.
//   The inverse DCT output is added to the output of the prediction.

namespace Gfx {

// https://developers.google.com/speed/webp/docs/riff_container#simple_file_format_lossy
// https://datatracker.ietf.org/doc/html/rfc6386#section-19 "Annex A: Bitstream Syntax"
ErrorOr<VP8Header> decode_webp_chunk_VP8_header(ReadonlyBytes vp8_data)
{
    if (vp8_data.size() < 10)
        return Error::from_string_literal("WebPImageDecoderPlugin: 'VP8 ' chunk too small");

    // FIXME: Eventually, this should probably call into LibVideo/VP8,
    // and image decoders should move into LibImageDecoders which depends on both LibGfx and LibVideo.
    // (LibVideo depends on LibGfx, so LibGfx can't depend on LibVideo itself.)

    // https://datatracker.ietf.org/doc/html/rfc6386#section-4 "Overview of Compressed Data Format"
    // "The decoder is simply presented with a sequence of compressed frames [...]
    //  The first frame presented to the decompressor is [...] a key frame.  [...]
    //  [E]very compressed frame has three or more pieces. It begins with an uncompressed data chunk comprising 10 bytes in the case of key frames"

    u8 const* data = vp8_data.data();

    // https://datatracker.ietf.org/doc/html/rfc6386#section-9.1 "Uncompressed Data Chunk"
    u32 frame_tag = data[0] | (data[1] << 8) | (data[2] << 16);
    bool is_key_frame = (frame_tag & 1) == 0; // https://www.rfc-editor.org/errata/eid5534
    u8 version = (frame_tag & 0xe) >> 1;
    bool show_frame = (frame_tag & 0x10) != 0;
    u32 size_of_first_partition = frame_tag >> 5;

    if (!is_key_frame)
        return Error::from_string_literal("WebPImageDecoderPlugin: 'VP8 ' chunk not a key frame");

    if (!show_frame)
        return Error::from_string_literal("WebPImageDecoderPlugin: 'VP8 ' chunk has invalid visibility for webp image");

    if (version > 3)
        return Error::from_string_literal("WebPImageDecoderPlugin: unknown version number in 'VP8 ' chunk");

    u32 start_code = data[3] | (data[4] << 8) | (data[5] << 16);
    if (start_code != 0x2a019d) // https://www.rfc-editor.org/errata/eid7370
        return Error::from_string_literal("WebPImageDecoderPlugin: 'VP8 ' chunk invalid start_code");

    // "The scaling specifications for each dimension are encoded as follows.
    //   0     | No upscaling (the most common case).
    //   1     | Upscale by 5/4.
    //   2     | Upscale by 5/3.
    //   3     | Upscale by 2."
    // This is a display-time operation and doesn't affect decoding."
    u16 width_and_horizontal_scale = data[6] | (data[7] << 8);
    u16 width = width_and_horizontal_scale & 0x3fff;
    u8 horizontal_scale = width_and_horizontal_scale >> 14;

    u16 heigth_and_vertical_scale = data[8] | (data[9] << 8);
    u16 height = heigth_and_vertical_scale & 0x3fff;
    u8 vertical_scale = heigth_and_vertical_scale >> 14;

    dbgln_if(WEBP_DEBUG, "version {}, show_frame {}, size_of_first_partition {}, width {}, horizontal_scale {}, height {}, vertical_scale {}",
        version, show_frame, size_of_first_partition, width, horizontal_scale, height, vertical_scale);

    if (vp8_data.size() < 10 + size_of_first_partition)
        return Error::from_string_literal("WebPImageDecoderPlugin: 'VP8 ' chunk too small for full first partition");

    return VP8Header { version, show_frame, size_of_first_partition, width, horizontal_scale, height, vertical_scale, vp8_data.slice(10, size_of_first_partition), vp8_data.slice(10 + size_of_first_partition) };
}

namespace {

// Reads n bits followed by a sign bit (0: positive, 1: negative).
i8 read_signed_literal(BooleanDecoder& decoder, u8 n)
{
    VERIFY(n <= 7);
    i8 i = decoder.read_literal(n);
    if (decoder.read_literal(1))
        i = -i;
    return i;
}

// https://datatracker.ietf.org/doc/html/rfc6386#section-19 "Annex A: Bitstream Syntax"
#define L(n) decoder.read_literal(n)
#define B(prob) decoder.read_bool(prob)
#define L_signed(n) read_signed_literal(decoder, n)

// https://datatracker.ietf.org/doc/html/rfc6386#section-9.3 Segment-Based Adjustments"
// https://datatracker.ietf.org/doc/html/rfc6386#section-19.2 "Frame Header"
enum class SegmentFeatureMode {
    // Spec 19.2 says 0 is delta, 1 absolute; spec 9.3 has it the other way round. 19.2 is correct.
    // https://www.rfc-editor.org/errata/eid7519
    DeltaValueMode = 0,
    AbsoluteValueMode = 1,

};
struct Segmentation {
    bool update_macroblock_segmentation_map { false };
    SegmentFeatureMode segment_feature_mode { SegmentFeatureMode::DeltaValueMode };

    i8 quantizer_update_value[4] {};
    i8 loop_filter_update_value[4] {};

    u8 macroblock_segment_tree_probabilities[3] = { 255, 255, 255 };
};
Segmentation decode_VP8_frame_header_segmentation(BooleanDecoder&);

// Also https://datatracker.ietf.org/doc/html/rfc6386#section-9.6 "Dequantization Indices"
struct QuantizationIndices {
    u8 y_ac { 0 };
    i8 y_dc_delta { 0 };

    i8 y2_dc_delta { 0 };
    i8 y2_ac_delta { 0 };

    i8 uv_dc_delta { 0 };
    i8 uv_ac_delta { 0 };
};
QuantizationIndices decode_VP8_frame_header_quantization_indices(BooleanDecoder&);

struct LoopFilterAdjustment {
    bool enable_loop_filter_adjustment { false };
    i8 ref_frame_delta[4] {};
    i8 mb_mode_delta[4] {};
};
LoopFilterAdjustment decode_VP8_frame_header_loop_filter_adjustment(BooleanDecoder&);

using CoefficientProbabilities = Prob[4][8][3][num_dct_tokens - 1];
ErrorOr<void> decode_VP8_frame_header_coefficient_probabilities(BooleanDecoder&, CoefficientProbabilities);

// https://datatracker.ietf.org/doc/html/rfc6386#section-15 "Loop Filter"
// "The first is a flag (filter_type) selecting the type of filter (normal or simple)"
enum class FilterType {
    Normal = 0,
    Simple = 1,
};

// https://datatracker.ietf.org/doc/html/rfc6386#section-9.2 "Color Space and Pixel Type (Key Frames Only)"
enum class ColorSpaceAndPixelType {
    YUV = 0,
    ReservedForFutureUse = 1,
};
enum class ClampingSpecification {
    DecoderMustClampTo0To255 = 0,
    NoClampingNecessary = 1,
};

// https://datatracker.ietf.org/doc/html/rfc6386#section-19.2 "Frame Header"
struct FrameHeader {
    ColorSpaceAndPixelType color_space {};
    ClampingSpecification clamping_type {};

    bool is_segmentation_enabled {};
    Segmentation segmentation {};

    FilterType filter_type {};
    u8 loop_filter_level {};
    u8 sharpness_level {};
    LoopFilterAdjustment loop_filter_adjustment {};

    u8 number_of_dct_partitions {};

    QuantizationIndices quantization_indices {};

    CoefficientProbabilities coefficient_probabilities;

    bool enable_skipping_of_macroblocks_containing_only_zero_coefficients {};
    u8 probability_skip_false;
};

ErrorOr<FrameHeader> decode_VP8_frame_header(BooleanDecoder& decoder)
{
    // https://datatracker.ietf.org/doc/html/rfc6386#section-19.2 "Frame Header"
    FrameHeader header;

    // In the VP8 spec, this is in an `if (key_frames)`, but webp files only have key frames.
    header.color_space = ColorSpaceAndPixelType { L(1) };
    header.clamping_type = ClampingSpecification { L(1) };
    dbgln_if(WEBP_DEBUG, "color_space {} clamping_type {}", (int)header.color_space, (int)header.clamping_type);

    // https://datatracker.ietf.org/doc/html/rfc6386#section-9.3 "Segment-Based Adjustments"
    header.is_segmentation_enabled = L(1);
    dbgln_if(WEBP_DEBUG, "segmentation_enabled {}", header.is_segmentation_enabled);

    if (header.is_segmentation_enabled)
        header.segmentation = decode_VP8_frame_header_segmentation(decoder);

    header.filter_type = FilterType { L(1) };
    header.loop_filter_level = L(6);
    header.sharpness_level = L(3);
    dbgln_if(WEBP_DEBUG, "filter_type {} loop_filter_level {} sharpness_level {}", (int)header.filter_type, header.loop_filter_level, header.sharpness_level);

    header.loop_filter_adjustment = decode_VP8_frame_header_loop_filter_adjustment(decoder);

    u8 log2_nbr_of_dct_partitions = L(2);
    dbgln_if(WEBP_DEBUG, "log2_nbr_of_dct_partitions {}", log2_nbr_of_dct_partitions);
    header.number_of_dct_partitions = 1 << log2_nbr_of_dct_partitions;

    header.quantization_indices = decode_VP8_frame_header_quantization_indices(decoder);

    // In the VP8 spec, this is in an `if (key_frames)` followed by a lengthy `else`, but webp files only have key frames.
    u8 refresh_entropy_probs = L(1); // Has no effect in webp files.
    dbgln_if(WEBP_DEBUG, "refresh_entropy_probs {}", refresh_entropy_probs);

    memcpy(header.coefficient_probabilities, DEFAULT_COEFFICIENT_PROBABILITIES, sizeof(header.coefficient_probabilities));
    TRY(decode_VP8_frame_header_coefficient_probabilities(decoder, header.coefficient_probabilities));

    // https://datatracker.ietf.org/doc/html/rfc6386#section-9.11 "Remaining Frame Header Data (Key Frame)"
    header.enable_skipping_of_macroblocks_containing_only_zero_coefficients = L(1);
    dbgln_if(WEBP_DEBUG, "mb_no_skip_coeff {}", header.enable_skipping_of_macroblocks_containing_only_zero_coefficients);
    if (header.enable_skipping_of_macroblocks_containing_only_zero_coefficients) {
        header.probability_skip_false = L(8);
        dbgln_if(WEBP_DEBUG, "prob_skip_false {}", header.probability_skip_false);
    }

    // In the VP8 spec, there is a length `if (!key_frames)` here, but webp files only have key frames.

    return header;
}

Segmentation decode_VP8_frame_header_segmentation(BooleanDecoder& decoder)
{
    // Corresponds to "update_segmentation()" in section 19.2 of the spec.
    Segmentation segmentation;

    segmentation.update_macroblock_segmentation_map = L(1);
    u8 update_segment_feature_data = L(1);

    dbgln_if(WEBP_DEBUG, "update_mb_segmentation_map {} update_segment_feature_data {}",
        segmentation.update_macroblock_segmentation_map, update_segment_feature_data);

    if (update_segment_feature_data) {
        segmentation.segment_feature_mode = static_cast<SegmentFeatureMode>(L(1));
        dbgln_if(WEBP_DEBUG, "segment_feature_mode {}", (int)segmentation.segment_feature_mode);

        for (int i = 0; i < 4; ++i) {
            u8 quantizer_update = L(1);
            dbgln_if(WEBP_DEBUG, "quantizer_update {}", quantizer_update);
            if (quantizer_update) {
                i8 quantizer_update_value = L_signed(7);
                dbgln_if(WEBP_DEBUG, "quantizer_update_value {}", quantizer_update_value);
                segmentation.quantizer_update_value[i] = quantizer_update_value;
            }
        }
        for (int i = 0; i < 4; ++i) {
            u8 loop_filter_update = L(1);
            dbgln_if(WEBP_DEBUG, "loop_filter_update {}", loop_filter_update);
            if (loop_filter_update) {
                i8 loop_filter_update_value = L_signed(6);
                dbgln_if(WEBP_DEBUG, "loop_filter_update_value {}", loop_filter_update_value);
                segmentation.loop_filter_update_value[i] = loop_filter_update_value;
            }
        }
    }

    if (segmentation.update_macroblock_segmentation_map) {
        // This reads mb_segment_tree_probs for https://datatracker.ietf.org/doc/html/rfc6386#section-10.
        for (int i = 0; i < 3; ++i) {
            u8 segment_prob_update = L(1);
            dbgln_if(WEBP_DEBUG, "segment_prob_update {}", segment_prob_update);
            if (segment_prob_update) {
                u8 segment_prob = L(8);
                dbgln_if(WEBP_DEBUG, "segment_prob {}", segment_prob);
                segmentation.macroblock_segment_tree_probabilities[i] = segment_prob;
            }
        }
    }

    return segmentation;
}

QuantizationIndices decode_VP8_frame_header_quantization_indices(BooleanDecoder& decoder)
{
    // Corresponds to "quant_indices()" in section 19.2 of the spec.
    QuantizationIndices quantization_indices;

    // "The first 7-bit index gives the dequantization table index for
    //  Y-plane AC coefficients, called yac_qi.  It is always coded and acts
    //  as a baseline for the other 5 quantization indices, each of which is
    //  represented by a delta from this baseline index."
    quantization_indices.y_ac = L(7);
    dbgln_if(WEBP_DEBUG, "y_ac_qi {}", quantization_indices.y_ac);

    auto read_delta = [&decoder](StringView name, i8* destination) -> void {
        u8 is_present = L(1);
        dbgln_if(WEBP_DEBUG, "{}_present {}", name, is_present);
        if (is_present) {
            i8 delta = L_signed(4);
            dbgln_if(WEBP_DEBUG, "{} {}", name, delta);
            *destination = delta;
        }
    };
    read_delta("y_dc_delta"sv, &quantization_indices.y_dc_delta);
    read_delta("y2_dc_delta"sv, &quantization_indices.y2_dc_delta);
    read_delta("y2_ac_delta"sv, &quantization_indices.y2_ac_delta);
    read_delta("uv_dc_delta"sv, &quantization_indices.uv_dc_delta);
    read_delta("uv_ac_delta"sv, &quantization_indices.uv_ac_delta);

    return quantization_indices;
}

LoopFilterAdjustment decode_VP8_frame_header_loop_filter_adjustment(BooleanDecoder& decoder)
{
    // Corresponds to "mb_lf_adjustments()" in section 19.2 of the spec.
    LoopFilterAdjustment adjustment;

    adjustment.enable_loop_filter_adjustment = L(1);
    if (adjustment.enable_loop_filter_adjustment) {
        u8 mode_ref_lf_delta_update = L(1);
        dbgln_if(WEBP_DEBUG, "mode_ref_lf_delta_update {}", mode_ref_lf_delta_update);
        if (mode_ref_lf_delta_update) {
            for (int i = 0; i < 4; ++i) {
                u8 ref_frame_delta_update_flag = L(1);
                dbgln_if(WEBP_DEBUG, "ref_frame_delta_update_flag {}", ref_frame_delta_update_flag);
                if (ref_frame_delta_update_flag) {
                    i8 delta = L_signed(6);
                    dbgln_if(WEBP_DEBUG, "delta {}", delta);
                    adjustment.ref_frame_delta[i] = delta;
                }
            }
            for (int i = 0; i < 4; ++i) {
                u8 mb_mode_delta_update_flag = L(1);
                dbgln_if(WEBP_DEBUG, "mb_mode_delta_update_flag {}", mb_mode_delta_update_flag);
                if (mb_mode_delta_update_flag) {
                    i8 delta = L_signed(6);
                    dbgln_if(WEBP_DEBUG, "delta {}", delta);
                    adjustment.mb_mode_delta[i] = delta;
                }
            }
        }
    }

    return adjustment;
}

ErrorOr<void> decode_VP8_frame_header_coefficient_probabilities(BooleanDecoder& decoder, CoefficientProbabilities coefficient_probabilities)
{
    // Corresponds to "token_prob_update()" in section 19.2 of the spec.
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            for (int k = 0; k < 3; k++) {
                for (int l = 0; l < 11; l++) {
                    // token_prob_update() says L(1) and L(8), but it's actually B(p) and L(8).
                    // https://datatracker.ietf.org/doc/html/rfc6386#section-13.4 "Token Probability Updates" describes it correctly.
                    if (B(COEFFICIENT_UPDATE_PROBABILITIES[i][j][k][l]))
                        coefficient_probabilities[i][j][k][l] = L(8);
                }
            }
        }
    }

    return {};
}

// https://datatracker.ietf.org/doc/html/rfc6386#section-8.1 "Tree Coding Implementation"
u8 tree_decode(BooleanDecoder& decoder, ReadonlySpan<TreeIndex> tree, ReadonlyBytes probabilities, TreeIndex initial_i = 0)
{
    TreeIndex i = initial_i;
    while (true) {
        u8 b = B(probabilities[i >> 1]);
        i = tree[i + b];
        if (i <= 0)
            return -i;
    }
}

// Similar to BlockContext in LibVideo/VP9/Context.h
struct MacroblockMetadata {
    // https://datatracker.ietf.org/doc/html/rfc6386#section-10 "Segment-Based Feature Adjustments"
    // Read only if `update_mb_segmentation_map` is set.
    u8 segment_id { 0 }; // 0, 1, 2, or 3. Fits in two bits.

    // https://datatracker.ietf.org/doc/html/rfc6386#section-11.1 "mb_skip_coeff"
    bool skip_coefficients { false };

    IntraMacroblockMode intra_y_mode;
    IntraMacroblockMode uv_mode;

    IntraBlockMode intra_b_modes[16];
};

ErrorOr<Vector<MacroblockMetadata>> decode_VP8_macroblock_metadata(BooleanDecoder& decoder, FrameHeader const& header, int macroblock_width, int macroblock_height)
{
    // https://datatracker.ietf.org/doc/html/rfc6386#section-19.3

    // Corresponds to "macroblock_header()" in section 19.3 of the spec.

    Vector<MacroblockMetadata> macroblock_metadata;

    // Key frames must use intra prediction, that is new macroblocks are predicted from old macroblocks in the same frame.
    // (Inter prediction on the other hand predicts new macroblocks from the corresponding macroblock in the previous frame.)

    // https://datatracker.ietf.org/doc/html/rfc6386#section-11.3 "Subblock Mode Contexts"
    // "For macroblocks on the top row or left edge of the image, some of
    //  the predictors will be non-existent.  Such predictors are taken
    //  to have had the value B_DC_PRED, which, perhaps conveniently,
    //  takes the value 0 in the enumeration above.
    //  A simple management scheme for these contexts might maintain a row
    //  of above predictors and four left predictors.  Before decoding the
    //  frame, the entire row is initialized to B_DC_PRED; before decoding
    //  each row of macroblocks, the four left predictors are also set to
    //  B_DC_PRED.  After decoding a macroblock, the bottom four subblock
    //  modes are copied into the row predictor (at the current position,
    //  which then advances to be above the next macroblock), and the
    //  right four subblock modes are copied into the left predictor."
    Vector<IntraBlockMode> above;
    TRY(above.try_resize(macroblock_width * 4)); // One per 4x4 subblock.

    // It's possible to not decode all macroblock metadata at once. Instead, this could for example decode one row of metadata,
    // then decode the coefficients for one row of macroblocks, convert that row to pixels, and then go on to the next row of macroblocks.
    // That'd require slightly less memory. But MacroblockMetadata is fairly small, and this way we can keep the context
    // (`above`, `left`) in stack variables instead of having to have a class for that. So keep it simple for now.
    for (int mb_y = 0; mb_y < macroblock_height; ++mb_y) {
        IntraBlockMode left[4] {};

        for (int mb_x = 0; mb_x < macroblock_width; ++mb_x) {
            MacroblockMetadata metadata;

            if (header.segmentation.update_macroblock_segmentation_map)
                metadata.segment_id = tree_decode(decoder, MACROBLOCK_SEGMENT_TREE, header.segmentation.macroblock_segment_tree_probabilities);

            if (header.enable_skipping_of_macroblocks_containing_only_zero_coefficients)
                metadata.skip_coefficients = B(header.probability_skip_false);

            int intra_y_mode = tree_decode(decoder, KEYFRAME_YMODE_TREE, KEYFRAME_YMODE_PROBABILITIES);
            metadata.intra_y_mode = (IntraMacroblockMode)intra_y_mode;

            // "If the Ymode is B_PRED, it is followed by a (tree-coded) mode for each of the 16 Y subblocks."
            if (intra_y_mode == B_PRED) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        // "The outer two dimensions of this array are indexed by the already-
                        //  coded subblock modes above and to the left of the current block,
                        //  respectively."
                        int A = above[mb_x * 4 + x];
                        int L = left[y];

                        auto intra_b_mode = static_cast<IntraBlockMode>(tree_decode(decoder, BLOCK_MODE_TREE, KEYFRAME_BLOCK_MODE_PROBABILITIES[A][L]));
                        metadata.intra_b_modes[y * 4 + x] = intra_b_mode;

                        above[mb_x * 4 + x] = intra_b_mode;
                        left[y] = intra_b_mode;
                    }
                }
            } else {
                VERIFY(intra_y_mode < B_PRED);
                constexpr IntraBlockMode b_mode_from_y_mode[] = { B_DC_PRED, B_VE_PRED, B_HE_PRED, B_TM_PRED };
                IntraBlockMode intra_b_mode = b_mode_from_y_mode[intra_y_mode];
                for (int i = 0; i < 4; ++i) {
                    above[mb_x * 4 + i] = intra_b_mode;
                    left[i] = intra_b_mode;
                }
            }

            metadata.uv_mode = (IntraMacroblockMode)tree_decode(decoder, UV_MODE_TREE, KEYFRAME_UV_MODE_PROBABILITIES);

            TRY(macroblock_metadata.try_append(metadata));
        }
    }

    return macroblock_metadata;
}

// Every macroblock stores:
// - One optional set of coefficients for Y2
// - 16 sets of Y coefficients for the 4x4 Y subblocks of the macroblock
// - 4 sets of U coefficients for the 2x2 U subblocks of the macroblock
// - 4 sets of V coefficients for the 2x2 V subblocks of the macroblock
// That's 24 or 25 sets of coefficients total. This struct identifies one of these sets by index.
// If a macroblock does not have Y2, then i goes from [1..25], else it goes [0..25].
struct CoefficientBlockIndex {
    int i;

    CoefficientBlockIndex(int i)
        : i(i)
    {
        VERIFY(i >= 0);
        VERIFY(i <= 25);
    }

    bool is_y2() const { return i == 0; }
    bool is_y() const { return i >= 1 && i <= 16; }
    bool is_u() const { return i >= 17 && i <= 20; }
    bool is_v() const { return i >= 21; }

    u8 sub_x() const
    {
        VERIFY(i > 0);
        if (i <= 16)
            return (i - 1) % 4;
        if (i <= 20)
            return (i - 17) % 2;
        return (i - 21) % 2;
    }

    u8 sub_y() const
    {
        VERIFY(i > 0);
        if (i <= 16)
            return (i - 1) / 4;
        if (i <= 20)
            return (i - 17) / 2;
        return (i - 21) / 2;
    }
};

int plane_index(CoefficientBlockIndex index, bool have_y2)
{
    // https://datatracker.ietf.org/doc/html/rfc6386#section-13.3 "Token Probabilities"
    // "o  0 - Y beginning at coefficient 1 (i.e., Y after Y2)
    //  o  1 - Y2
    //  o  2 - U or V
    //  o  3 - Y beginning at coefficient 0 (i.e., Y in the absence of Y2)."
    if (index.is_y2())
        return 1;
    if (index.is_u() || index.is_v())
        return 2;
    if (have_y2)
        return 0;
    return 3;
}

i16 coefficient_value_for_token(BooleanDecoder& decoder, u8 token)
{
    // Implements the second half of https://datatracker.ietf.org/doc/html/rfc6386#section-13.2 "Coding of Individual Coefficient Values"
    i16 v = static_cast<i16>(token); // For DCT_0 to DCT4

    if (token >= dct_cat1 && token <= dct_cat6) {
        static int constexpr starts[] = { 5, 7, 11, 19, 35, 67 };
        static int constexpr bits[] = { 1, 2, 3, 4, 5, 11 };

        static Prob constexpr Pcat1[] = { 159 };
        static Prob constexpr Pcat2[] = { 165, 145 };
        static Prob constexpr Pcat3[] = { 173, 148, 140 };
        static Prob constexpr Pcat4[] = { 176, 155, 140, 135 };
        static Prob constexpr Pcat5[] = { 180, 157, 141, 134, 130 };
        static Prob constexpr Pcat6[] = { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129 };
        static Prob const* const Pcats[] = { Pcat1, Pcat2, Pcat3, Pcat4, Pcat5, Pcat6 };

        v = 0;

        // This loop corresponds to `DCTextra` in the spec in section 13.2.
        for (int i = 0; i < bits[token - dct_cat1]; ++i)
            v = (v << 1) | decoder.read_bool(Pcats[token - dct_cat1][i]);

        v += starts[token - dct_cat1];
    }

    if (v) {
        if (decoder.read_bool(128))
            v = -v;
    }

    return v;
}

i16 dequantize_value(i16 value, bool is_dc, QuantizationIndices const& quantization_indices, Segmentation const& segmentation, int segment_id, CoefficientBlockIndex index)
{
    // https://datatracker.ietf.org/doc/html/rfc6386#section-9.6 "Dequantization Indices"
    // "before inverting the transform, each decoded coefficient
    //  is multiplied by one of six dequantization factors, the choice of
    //  which depends on the plane (Y, chroma = U or V, Y2) and coefficient
    //  position (DC = coefficient 0, AC = coefficients 1-15).  The six
    //  values are specified using 7-bit indices into six corresponding fixed
    //  tables (the tables are given in Section 14)."
    // Section 14 then lists two (!) fixed tables (which are in WebPLoaderLossyTables.h)

    // "Lookup values from the above two tables are directly used in the DC
    //  and AC coefficients in Y1, respectively.  For Y2 and chroma, values
    //  from the above tables undergo either scaling or clamping before the
    //  multiplies.  Details regarding these scaling and clamping processes
    //  can be found in related lookup functions in dixie.c (Section 20.4)."
    // Apparently spec writing became too much work at this point. In section 20.4, in dequant_init():
    // * For y2, the output (!) of dc_qlookup is multiplied by 2, the output of ac_qlookup is multiplied by 155 / 100
    // * Also for y2, ac_qlookup is at least 8 for lower table entries
    // * For uv, the dc_qlookup index is clamped to 117 (instead of 127 for everything else)
    //   (or, alternatively, the value is clamped to 132 at most)

    int y_ac_base = quantization_indices.y_ac;
    if (segmentation.update_macroblock_segmentation_map) {
        if (segmentation.segment_feature_mode == SegmentFeatureMode::DeltaValueMode)
            y_ac_base += segmentation.quantizer_update_value[segment_id];
        else
            y_ac_base = segmentation.quantizer_update_value[segment_id];
    }

    int dequantization_index;
    if (index.is_y2())
        dequantization_index = y_ac_base + (is_dc ? quantization_indices.y2_dc_delta : quantization_indices.y2_ac_delta);
    else if (index.is_u() || index.is_v())
        dequantization_index = y_ac_base + (is_dc ? quantization_indices.uv_dc_delta : quantization_indices.uv_ac_delta);
    else
        dequantization_index = is_dc ? (y_ac_base + quantization_indices.y_dc_delta) : y_ac_base;

    // clamp index
    if ((index.is_u() || index.is_v()) && is_dc)
        dequantization_index = clamp(dequantization_index, 0, 117);
    else
        dequantization_index = clamp(dequantization_index, 0, 127);

    // "the multiplies are computed and stored using 16-bit signed integers."
    i16 dequantization_factor;
    if (is_dc)
        dequantization_factor = (i16)dc_qlookup[dequantization_index];
    else
        dequantization_factor = (i16)ac_qlookup[dequantization_index];

    if (index.is_y2()) {
        if (is_dc)
            dequantization_factor *= 2;
        else
            dequantization_factor = max((dequantization_factor * 155) / 100, 8);
    }

    return dequantization_factor * value;
}

// Reading macroblock coefficients requires needing to know if the block to the left and above the current macroblock
// has non-zero coefficients. This stores that state.
struct CoefficientReadingContext {
    // Store if each plane has nonzero coefficients in the block above and to the left of the current block.
    Vector<bool> y2_above;
    Vector<bool> y_above;
    Vector<bool> u_above;
    Vector<bool> v_above;

    bool y2_left {};
    bool y_left[4] {};
    bool u_left[2] {};
    bool v_left[2] {};

    ErrorOr<void> initialize(int macroblock_width)
    {
        TRY(y2_above.try_resize(macroblock_width));
        TRY(y_above.try_resize(macroblock_width * 4));
        TRY(u_above.try_resize(macroblock_width * 2));
        TRY(v_above.try_resize(macroblock_width * 2));
        return {};
    }

    void start_new_row()
    {
        y2_left = false;
        for (bool& b : y_left)
            b = false;
        for (bool& b : u_left)
            b = false;
        for (bool& b : v_left)
            b = false;
    }

    bool& was_above_nonzero(CoefficientBlockIndex index, int mb_x)
    {
        if (index.is_y2())
            return y2_above[mb_x];
        if (index.is_u())
            return u_above[mb_x * 2 + index.sub_x()];
        if (index.is_v())
            return v_above[mb_x * 2 + index.sub_x()];
        return y_above[mb_x * 4 + index.sub_x()];
    }
    bool was_above_nonzero(CoefficientBlockIndex index, int mb_x) const { return const_cast<CoefficientReadingContext&>(*this).was_above_nonzero(index, mb_x); }

    bool& was_left_nonzero(CoefficientBlockIndex index)
    {
        if (index.is_y2())
            return y2_left;
        if (index.is_u())
            return u_left[index.sub_y()];
        if (index.is_v())
            return v_left[index.sub_y()];
        return y_left[index.sub_y()];
    }
    bool was_left_nonzero(CoefficientBlockIndex index) const { return const_cast<CoefficientReadingContext&>(*this).was_left_nonzero(index); }

    void update(CoefficientBlockIndex index, int mb_x, bool subblock_has_nonzero_coefficients)
    {
        was_above_nonzero(index, mb_x) = subblock_has_nonzero_coefficients;
        was_left_nonzero(index) = subblock_has_nonzero_coefficients;
    }
};

using Coefficients = i16[16];

// Returns if any non-zero coefficients were read.
bool read_coefficent_block(BooleanDecoder& decoder, Coefficients out_coefficients, CoefficientBlockIndex block_index, CoefficientReadingContext& coefficient_reading_context, int mb_x, bool have_y2, int segment_id, FrameHeader const& header)
{
    // Corresponds to `residual_block()` in https://datatracker.ietf.org/doc/html/rfc6386#section-19.3,
    // but also does dequantization of the stored values.
    // "firstCoeff is 1 for luma blocks of macroblocks containing Y2 subblock; otherwise 0"
    int firstCoeff = have_y2 && block_index.is_y() ? 1 : 0;
    i16 last_decoded_value = num_dct_tokens; // Start with an invalid value

    bool subblock_has_nonzero_coefficients = false;

    for (int j = firstCoeff; j < 16; ++j) {
        // https://datatracker.ietf.org/doc/html/rfc6386#section-13.2 "Coding of Individual Coefficient Values"
        // https://datatracker.ietf.org/doc/html/rfc6386#section-13.3 "Token Probabilities"

        // "Working from the outside in, the outermost dimension is indexed by
        //  the type of plane being decoded"
        int plane = plane_index(block_index, have_y2);

        // "The next dimension is selected by the position of the coefficient
        //  being decoded.  That position, c, steps by ones up to 15, starting
        //  from zero for block types 1, 2, or 3 and starting from one for block
        //  type 0.  The second array index is then"
        // "block type" here seems to refer to the "type of plane" in the previous paragraph.
        static int constexpr coeff_bands[16] = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7 };
        int band = coeff_bands[j];

        // "The third dimension is the trickiest."
        int tricky = 0;

        // "For the first coefficient (DC, unless the block type is 0), we
        //  consider the (already encoded) blocks within the same plane (Y2, Y,
        //  U, or V) above and to the left of the current block.  The context
        //  index is then the number (0, 1, or 2) of these blocks that had at
        //  least one non-zero coefficient in their residue record.  Specifically
        //  for Y2, because macroblocks above and to the left may or may not have
        //  a Y2 block, the block above is determined by the most recent
        //  macroblock in the same column that has a Y2 block, and the block to
        //  the left is determined by the most recent macroblock in the same row
        //  that has a Y2 block.
        //  [...]
        //  As with other contexts used by VP8, the "neighboring block" context
        //  described here needs a special definition for subblocks lying along
        //  the top row or left edge of the frame.  These "non-existent"
        //  predictors above and to the left of the image are simply taken to be
        //  empty -- that is, taken to contain no non-zero coefficients."
        if (j == firstCoeff) {
            bool was_left_nonzero = coefficient_reading_context.was_left_nonzero(block_index);
            bool was_above_nonzero = coefficient_reading_context.was_above_nonzero(block_index, mb_x);
            tricky = static_cast<int>(was_left_nonzero) + static_cast<int>(was_above_nonzero);
        }
        // "Beyond the first coefficient, the context index is determined by the
        //  absolute value of the most recently decoded coefficient (necessarily
        //  within the current block) and is 0 if the last coefficient was a
        //  zero, 1 if it was plus or minus one, and 2 if its absolute value
        //  exceeded one."
        else {
            if (last_decoded_value == 0)
                tricky = 0;
            else if (last_decoded_value == 1 || last_decoded_value == -1)
                tricky = 1;
            else
                tricky = 2;
        }

        // "In general, all DCT coefficients are decoded using the same tree.
        //  However, if the preceding coefficient is a DCT_0, decoding will skip
        //  the first branch, since it is not possible for dct_eob to follow a
        //  DCT_0."
        u8 token = tree_decode(decoder, COEFFICIENT_TREE, header.coefficient_probabilities[plane][band][tricky], last_decoded_value == DCT_0 ? 2 : 0);
        if (token == dct_eob)
            break;

        i16 v = coefficient_value_for_token(decoder, token);

        if (v) {
            // Subblock has non-0 coefficients. Store that, so that `tricky` on the next subblock is initialized correctly.
            subblock_has_nonzero_coefficients = true;
        }

        // last_decoded_value is used for setting `tricky`. It needs to be set to the last decoded token, not to the last dequantized value.
        last_decoded_value = v;

        i16 dequantized_value = dequantize_value(v, j == 0, header.quantization_indices, header.segmentation, segment_id, block_index);

        static int constexpr Zigzag[] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
        out_coefficients[Zigzag[j]] = dequantized_value;
    }

    return subblock_has_nonzero_coefficients;
}

struct MacroblockCoefficients {
    Coefficients y_coeffs[16] {};
    Coefficients u_coeffs[4] {};
    Coefficients v_coeffs[4] {};
};

MacroblockCoefficients read_macroblock_coefficients(BooleanDecoder& decoder, FrameHeader const& header, CoefficientReadingContext& coefficient_reading_context, MacroblockMetadata const& metadata, int mb_x)
{
    // Corresponds to `residual_data()` in https://datatracker.ietf.org/doc/html/rfc6386#section-19.3,
    // but also does the inverse walsh-hadamard transform if a Y2 block is present.

    MacroblockCoefficients coefficients;
    Coefficients y2_coeffs {};

    // "firstCoeff is 1 for luma blocks of macroblocks containing Y2 subblock; otherwise 0"

    // https://datatracker.ietf.org/doc/html/rfc6386#section-13

    // "For all intra- and inter-prediction modes apart from B_PRED (intra:
    //  whose Y subblocks are independently predicted) and SPLITMV (inter),
    //  each macroblock's residue record begins with the Y2 component of the
    //  residue, coded using a WHT.  B_PRED and SPLITMV coded macroblocks
    //  omit this WHT and specify the 0th DCT coefficient in each of the 16 Y
    //  subblocks."
    bool have_y2 = metadata.intra_y_mode != B_PRED;

    // "for Y2, because macroblocks above and to the left may or may not have
    //  a Y2 block, the block above is determined by the most recent
    //  macroblock in the same column that has a Y2 block, and the block to
    //  the left is determined by the most recent macroblock in the same row
    //  that has a Y2 block."
    // We only write to y2_above / y2_left when it's present, so we don't need to do any explicit work to get the right behavior.

    // "After the optional Y2 block, the residue record continues with 16
    //  DCTs for the Y subblocks, followed by 4 DCTs for the U subblocks,
    //  ending with 4 DCTs for the V subblocks.  The subblocks occur in the
    //  usual order."

    /* (1 Y2)?, 16 Y, 4 U, 4 V */
    for (int i = have_y2 ? 0 : 1; i < 25; ++i) {
        CoefficientBlockIndex block_index { i };

        bool subblock_has_nonzero_coefficients = false;

        if (!metadata.skip_coefficients) {
            i16* to_read;
            if (block_index.is_y2())
                to_read = y2_coeffs;
            else if (block_index.is_u())
                to_read = coefficients.u_coeffs[i - 17];
            else if (block_index.is_v())
                to_read = coefficients.v_coeffs[i - 21];
            else // Y
                to_read = coefficients.y_coeffs[i - 1];
            subblock_has_nonzero_coefficients = read_coefficent_block(decoder, to_read, block_index, coefficient_reading_context, mb_x, have_y2, metadata.segment_id, header);
        }

        coefficient_reading_context.update(block_index, mb_x, subblock_has_nonzero_coefficients);
    }

    // https://datatracker.ietf.org/doc/html/rfc6386#section-14.2 "Inverse Transforms"
    // "If the Y2 residue block exists (i.e., the macroblock luma mode is not
    //  SPLITMV or B_PRED), it is inverted first (using the inverse WHT) and
    //  the element of the result at row i, column j is used as the 0th
    //  coefficient of the Y subblock at position (i, j), that is, the Y
    //  subblock whose index is (i * 4) + j."
    if (have_y2) {
        Coefficients wht_output;
        vp8_short_inv_walsh4x4_c(y2_coeffs, wht_output);
        for (size_t i = 0; i < 16; ++i)
            coefficients.y_coeffs[i][0] = wht_output[i];
    }

    return coefficients;
}

template<int N>
void predict_macroblock(Bytes prediction, IntraMacroblockMode mode, int mb_x, int mb_y, ReadonlyBytes left, ReadonlyBytes above, u8 truemotion_corner)
{
    // https://datatracker.ietf.org/doc/html/rfc6386#section-12.2 "Chroma Prediction"
    // (Also used for the DC_PRED, H_PRED, V_PRED, TM_PRED for luma prediction.)
    if (mode == DC_PRED) {
        if (mb_x == 0 && mb_y == 0) {
            for (size_t i = 0; i < N * N; ++i)
                prediction[i] = 128;
        } else {
            int sum = 0, n = 0;
            if (mb_x > 0) {
                for (int i = 0; i < N; ++i)
                    sum += left[i];
                n += N;
            }
            if (mb_y > 0) {
                for (int i = 0; i < N; ++i)
                    sum += above[mb_x * N + i];
                n += N;
            }
            u8 average = (sum + n / 2) / n;
            for (size_t i = 0; i < N * N; ++i)
                prediction[i] = average;
        }
    } else if (mode == H_PRED) {
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                prediction[y * N + x] = left[y];
    } else if (mode == V_PRED) {
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                prediction[y * N + x] = above[mb_x * N + x];
    } else {
        VERIFY(mode == TM_PRED);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                prediction[y * N + x] = clamp(left[y] + above[mb_x * N + x] - truemotion_corner, 0, 255);
    }
}

void predict_y_subblock(Bytes y_prediction, IntraBlockMode mode, int x, int y, ReadonlyBytes left, ReadonlyBytes above, u8 corner)
{
    // https://datatracker.ietf.org/doc/html/rfc6386#section-12.3 "Luma Prediction"
    // Roughly corresponds to "subblock_intra_predict()" in the spec.
    auto weighted_average = [](u8 x, u8 y, u8 z) { return (x + 2 * y + z + 2) / 4; };
    auto average = [](u8 x, u8 y) { return (x + y + 1) / 2; };

    auto at = [&y_prediction, y, x](int px, int py) -> u8& { return y_prediction[(4 * y + py) * 16 + 4 * x + px]; };

    if (mode == B_DC_PRED) {
        // The spec text says this is like DC_PRED, but predict_dc_nxn() in the sample implementation doesn't do the "oob isn't read" part.
        int sum = 0, n = 8;
        for (int i = 0; i < 4; ++i)
            sum += left[i] + above[i];
        u8 average = (sum + n / 2) / n;
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px)
                y_prediction[(4 * y + py) * 16 + 4 * x + px] = average;
    } else if (mode == B_TM_PRED) {
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px)
                y_prediction[(4 * y + py) * 16 + 4 * x + px] = clamp(left[py] + above[px] - corner, 0, 255);
    } else if (mode == B_VE_PRED) {
        // The spec text says this is like V_PRED, but the sample implementation shows it does weighted averages (unlike V_PRED).
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px) {
                auto top_left = (px > 0 ? above[px - 1] : corner);
                y_prediction[(4 * y + py) * 16 + 4 * x + px] = weighted_average(top_left, above[px], above[px + 1]);
            }
    } else if (mode == B_HE_PRED) {
        // The spec text says this is like H_PRED, but the sample implementation shows it does weighted averages (unlike H_PRED).
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px) {
                if (py == 0) {
                    y_prediction[(4 * y + py) * 16 + 4 * x + px] = weighted_average(corner, left[py], left[py + 1]);
                } else if (py == 3) {
                    /* Bottom row is exceptional because L[4] does not exist */
                    y_prediction[(4 * y + py) * 16 + 4 * x + px] = weighted_average(left[2], left[3], left[3]);
                } else {
                    y_prediction[(4 * y + py) * 16 + 4 * x + px] = weighted_average(left[py - 1], left[py], left[py + 1]);
                }
            }
    } else if (mode == B_LD_PRED) {
        // this is 45-deg prediction from above, going left-down (i.e. isochromes on -1/+1 diags)
        at(0, 0) = weighted_average(above[0], above[1], above[2]);
        at(0, 1) = at(1, 0) = weighted_average(above[1], above[2], above[3]);
        at(0, 2) = at(1, 1) = at(2, 0) = weighted_average(above[2], above[3], above[4]);
        at(0, 3) = at(1, 2) = at(2, 1) = at(3, 0) = weighted_average(above[3], above[4], above[5]);
        at(1, 3) = at(2, 2) = at(3, 1) = weighted_average(above[4], above[5], above[6]);
        at(2, 3) = at(3, 2) = weighted_average(above[5], above[6], above[7]);
        at(3, 3) = weighted_average(above[6], above[7], above[7]); // intentionally 6, 7, 7
    } else if (mode == B_RD_PRED) {
        // this is 45-deg prediction from above / left, going right-down (i.e. isochromes on +1/+1 diags)
        at(0, 3) = weighted_average(left[3], left[2], left[1]);
        at(0, 2) = at(1, 3) = weighted_average(left[2], left[1], left[0]);
        at(0, 1) = at(1, 2) = at(2, 3) = weighted_average(left[1], left[0], corner);
        at(0, 0) = at(1, 1) = at(2, 2) = at(3, 3) = weighted_average(left[0], corner, above[0]);
        at(1, 0) = at(2, 1) = at(3, 2) = weighted_average(corner, above[0], above[1]);
        at(2, 0) = at(3, 1) = weighted_average(above[0], above[1], above[2]);
        at(3, 0) = weighted_average(above[1], above[2], above[3]);
    } else if (mode == B_VR_PRED) {
        // this is 22.5-deg prediction
        at(0, 3) = weighted_average(left[2], left[1], left[0]);
        at(0, 2) = weighted_average(left[1], left[0], corner);
        at(1, 3) = at(0, 1) = weighted_average(left[0], corner, above[0]);
        at(1, 2) = at(0, 0) = average(corner, above[0]);
        at(2, 3) = at(1, 1) = weighted_average(corner, above[0], above[1]);
        at(2, 2) = at(1, 0) = average(above[0], above[1]);
        at(3, 3) = at(2, 1) = weighted_average(above[0], above[1], above[2]);
        at(3, 2) = at(2, 0) = average(above[1], above[2]);
        at(3, 1) = weighted_average(above[1], above[2], above[3]);
        at(3, 0) = average(above[2], above[3]);
    } else if (mode == B_VL_PRED) {
        // this is 22.5-deg prediction
        at(0, 0) = average(above[0], above[1]);
        at(0, 1) = weighted_average(above[0], above[1], above[2]);
        at(0, 2) = at(1, 0) = average(above[1], above[2]);
        at(1, 1) = at(0, 3) = weighted_average(above[1], above[2], above[3]);
        at(1, 2) = at(2, 0) = average(above[2], above[3]);
        at(1, 3) = at(2, 1) = weighted_average(above[2], above[3], above[4]);
        at(2, 2) = at(3, 0) = average(above[3], above[4]);
        at(2, 3) = at(3, 1) = weighted_average(above[3], above[4], above[5]);
        /* Last two values do not strictly follow the pattern. */
        at(3, 2) = weighted_average(above[4], above[5], above[6]);
        at(3, 3) = weighted_average(above[5], above[6], above[7]);
    } else if (mode == B_HD_PRED) {
        // this is 22.5-deg prediction
        at(0, 3) = average(left[3], left[2]);
        at(1, 3) = weighted_average(left[3], left[2], left[1]);
        at(0, 2) = at(2, 3) = average(left[2], left[1]);
        at(1, 2) = at(3, 3) = weighted_average(left[2], left[1], left[0]);
        at(2, 2) = at(0, 1) = average(left[1], left[0]);
        at(3, 2) = at(1, 1) = weighted_average(left[1], left[0], corner);
        at(2, 1) = at(0, 0) = average(left[0], corner);
        at(3, 1) = at(1, 0) = weighted_average(left[0], corner, above[0]);
        at(2, 0) = weighted_average(corner, above[0], above[1]);
        at(3, 0) = weighted_average(above[0], above[1], above[2]);
    } else {
        VERIFY(mode == B_HU_PRED);
        // this is 22.5-deg prediction
        at(0, 0) = average(left[0], left[1]);
        at(1, 0) = weighted_average(left[0], left[1], left[2]);
        at(2, 0) = at(0, 1) = average(left[1], left[2]);
        at(3, 0) = at(1, 1) = weighted_average(left[1], left[2], left[3]);
        at(2, 1) = at(0, 2) = average(left[2], left[3]);
        at(3, 1) = at(1, 2) = weighted_average(left[2], left[3], left[3]); // Intentionally 2, 3, 3
        /* Not possible to follow pattern for much of the bottom
           row because no (nearby) already-constructed pixels lie
           on the diagonals in question. */
        at(2, 2) = at(3, 2) = at(0, 3) = at(1, 3) = at(2, 3) = at(3, 3) = left[3];
    }
}

template<int N>
void add_idct_to_prediction(Bytes prediction, Coefficients coefficients, int x, int y)
{
    Coefficients idct_output;
    short_idct4x4llm_c(coefficients, idct_output, 4 * sizeof(i16));

    // https://datatracker.ietf.org/doc/html/rfc6386#section-14.5 "Summation of Predictor and Residue"
    // FIXME: Could omit the clamp() call if FrameHeader.clamping_type == ClampingSpecification::NoClampingNecessary.
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            u8& p = prediction[(4 * y + py) * N + (4 * x + px)];
            p = clamp(p + idct_output[py * 4 + px], 0, 255);
        }
    }
}

template<int N>
void process_macroblock(Bytes output, IntraMacroblockMode mode, int mb_x, int mb_y, ReadonlyBytes left, ReadonlyBytes above, u8 truemotion_corner, Coefficients coefficients_array[])
{
    predict_macroblock<4 * N>(output, mode, mb_x, mb_y, left, above, truemotion_corner);

    // https://datatracker.ietf.org/doc/html/rfc6386#section-14.4 "Implementation of the DCT Inversion"
    // Loop over the 4x4 subblocks
    for (int y = 0, i = 0; y < N; ++y)
        for (int x = 0; x < N; ++x, ++i)
            add_idct_to_prediction<4 * N>(output, coefficients_array[i], x, y);
}

void process_subblocks(Bytes y_output, MacroblockMetadata const& metadata, int mb_x, ReadonlyBytes predicted_y_left, ReadonlyBytes predicted_y_above, u8 y_truemotion_corner, Coefficients coefficients_array[], int macroblock_width)
{
    // Loop over the 4x4 subblocks
    for (int y = 0, i = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x, ++i) {
            u8 corner = y_truemotion_corner;
            if (x > 0 && y == 0)
                corner = predicted_y_above[mb_x * 16 + 4 * x - 1];
            else if (x > 0 && y > 0)
                corner = y_output[(4 * y - 1) * 16 + 4 * x - 1];
            else if (x == 0 && y > 0)
                corner = predicted_y_left[4 * y - 1];

            u8 left[4], above[8];
            for (int i = 0; i < 4; ++i) {
                if (x == 0)
                    left[i] = predicted_y_left[4 * y + i];
                else
                    left[i] = y_output[(4 * y + i) * 16 + 4 * x - 1];
            }
            // Subblock prediction can read 8 pixels above the block.
            // For rightmost subblocks, the right 4 pixels there aren't initialized yet, so those get the 4 pixels to the right above the macroblock.
            // For the rightmost macroblock, there's no macroblock to its right, so there they get the rightmost pixel above.
            // But in the 0th row, there's no pixel above, so there they become 127.
            for (int i = 0; i < 8; ++i) {
                if (x == 3 && i >= 4) {                 // rightmost subblock, 4 right pixels?
                    if (mb_x == macroblock_width - 1) { // rightmost macroblock
                        // predicted_y_above is initialized to 127 above the first row, so no need for an explicit branch for mb_y == 0.
                        above[i] = predicted_y_above[mb_x * 16 + 4 * x + 3];
                    } else {
                        above[i] = predicted_y_above[mb_x * 16 + 4 * x + i];
                    }
                } else if (y == 0) {
                    above[i] = predicted_y_above[mb_x * 16 + 4 * x + i];
                } else {
                    above[i] = y_output[(4 * y - 1) * 16 + 4 * x + i];
                }
            }

            predict_y_subblock(y_output, metadata.intra_b_modes[y * 4 + x], x, y, left, above, corner);

            // Have to do IDCT summation here, since its results affect prediction of next subblock already.
            add_idct_to_prediction<16>(y_output, coefficients_array[4 * y + x], x, y);
        }
    }
}

void convert_yuv_to_rgb(Bitmap& bitmap, int mb_x, int mb_y, ReadonlyBytes y_data, ReadonlyBytes u_data, ReadonlyBytes v_data)
{
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            u8 Y = y_data[y * 16 + x];

            // FIXME: Could do nicer upsampling than just nearest neighbor
            u8 U = u_data[(y / 2) * 8 + x / 2];
            u8 V = v_data[(y / 2) * 8 + x / 2];

            // XXX: These numbers are from the fixed-point values in libwebp's yuv.h. There's probably a better reference somewhere.
            int r = 1.1655 * Y + 1.596 * V - 222.4;
            int g = 1.1655 * Y - 0.3917 * U - 0.8129 * V + 136.0625;
            int b = 1.1655 * Y + 2.0172 * U - 276.33;

            bitmap.scanline(mb_y * 16 + y)[mb_x * 16 + x] = Color(clamp(r, 0, 255), clamp(g, 0, 255), clamp(b, 0, 255)).value();
        }
    }
}

ErrorOr<void> decode_VP8_image_data(Gfx::Bitmap& bitmap, FrameHeader const& header, Vector<ReadonlyBytes> data_partitions, int macroblock_width, int macroblock_height, Vector<MacroblockMetadata> const& macroblock_metadata)
{

    Vector<BooleanDecoder> streams;
    for (auto data : data_partitions) {
        auto decoder = TRY(BooleanDecoder::initialize(data));
        TRY(streams.try_append(move(decoder)));
    }

    CoefficientReadingContext coefficient_reading_context;
    TRY(coefficient_reading_context.initialize(macroblock_width));

    Vector<u8> predicted_y_above;
    TRY(predicted_y_above.try_resize(macroblock_width * 16));
    for (size_t i = 0; i < predicted_y_above.size(); ++i)
        predicted_y_above[i] = 127;

    Vector<u8> predicted_u_above;
    TRY(predicted_u_above.try_resize(macroblock_width * 8));
    for (size_t i = 0; i < predicted_u_above.size(); ++i)
        predicted_u_above[i] = 127;

    Vector<u8> predicted_v_above;
    TRY(predicted_v_above.try_resize(macroblock_width * 8));
    for (size_t i = 0; i < predicted_v_above.size(); ++i)
        predicted_v_above[i] = 127;

    for (int mb_y = 0, macroblock_index = 0; mb_y < macroblock_height; ++mb_y) {
        BooleanDecoder& decoder = streams[mb_y % streams.size()];

        coefficient_reading_context.start_new_row();

        u8 predicted_y_left[16] { 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129 };
        u8 predicted_u_left[8] { 129, 129, 129, 129, 129, 129, 129, 129 };
        u8 predicted_v_left[8] { 129, 129, 129, 129, 129, 129, 129, 129 };

        // The spec doesn't say if this should be 127, 129, or something else.
        // But ReconstructRow in frame_dec.c in libwebp suggests 129.
        u8 y_truemotion_corner = 129;
        u8 u_truemotion_corner = 129;
        u8 v_truemotion_corner = 129;

        for (int mb_x = 0; mb_x < macroblock_width; ++mb_x, ++macroblock_index) {
            auto const& metadata = macroblock_metadata[macroblock_index];

            auto coefficients = read_macroblock_coefficients(decoder, header, coefficient_reading_context, metadata, mb_x);

            u8 y_data[16 * 16] {};
            if (metadata.intra_y_mode == B_PRED)
                process_subblocks(y_data, metadata, mb_x, predicted_y_left, predicted_y_above, y_truemotion_corner, coefficients.y_coeffs, macroblock_width);
            else
                process_macroblock<4>(y_data, metadata.intra_y_mode, mb_x, mb_y, predicted_y_left, predicted_y_above, y_truemotion_corner, coefficients.y_coeffs);

            u8 u_data[8 * 8] {};
            process_macroblock<2>(u_data, metadata.uv_mode, mb_x, mb_y, predicted_u_left, predicted_u_above, u_truemotion_corner, coefficients.u_coeffs);

            u8 v_data[8 * 8] {};
            process_macroblock<2>(v_data, metadata.uv_mode, mb_x, mb_y, predicted_v_left, predicted_v_above, v_truemotion_corner, coefficients.v_coeffs);

            // FIXME: insert loop filtering here

            convert_yuv_to_rgb(bitmap, mb_x, mb_y, y_data, u_data, v_data);

            y_truemotion_corner = predicted_y_above[mb_x * 16 + 15];
            for (int i = 0; i < 16; ++i)
                predicted_y_left[i] = y_data[15 + i * 16];
            for (int i = 0; i < 16; ++i)
                predicted_y_above[mb_x * 16 + i] = y_data[15 * 16 + i];

            u_truemotion_corner = predicted_u_above[mb_x * 8 + 7];
            for (int i = 0; i < 8; ++i)
                predicted_u_left[i] = u_data[7 + i * 8];
            for (int i = 0; i < 8; ++i)
                predicted_u_above[mb_x * 8 + i] = u_data[7 * 8 + i];

            v_truemotion_corner = predicted_v_above[mb_x * 8 + 7];
            for (int i = 0; i < 8; ++i)
                predicted_v_left[i] = v_data[7 + i * 8];
            for (int i = 0; i < 8; ++i)
                predicted_v_above[mb_x * 8 + i] = v_data[7 * 8 + i];
        }
    }

    for (auto& decoder : streams)
        TRY(decoder.finish_decode());

    return {};
}

static ErrorOr<Vector<ReadonlyBytes>> split_data_partitions(ReadonlyBytes second_partition, u8 number_of_dct_partitions)
{
    Vector<ReadonlyBytes> data_partitions;
    // https://datatracker.ietf.org/doc/html/rfc6386#section-9.5 "Token Partition and Partition Data Offsets"
    // "If the number of data partitions is
    //  greater than 1, the size of each partition (except the last) is
    //  written in 3 bytes (24 bits).  The size of the last partition is the
    //  remainder of the data not used by any of the previous partitions.
    //  The partitioned data are consecutive in the bitstream, so the size
    //  can also be used to calculate the offset of each partition."
    // In practice, virtually all lossy webp files have a single data partition.
    VERIFY(number_of_dct_partitions >= 1);
    VERIFY(number_of_dct_partitions <= 8);

    size_t sizes_size = (number_of_dct_partitions - 1) * 3;
    if (second_partition.size() < sizes_size)
        return Error::from_string_literal("WebPImageDecoderPlugin: not enough data for partition sizes");

    ReadonlyBytes sizes = second_partition.slice(0, sizes_size);
    ReadonlyBytes data = second_partition.slice(sizes_size);

    for (int i = 0; i < number_of_dct_partitions - 1; ++i) {
        u32 partition_size = sizes[0] | (sizes[1] << 8) | (sizes[2] << 16);
        dbgln_if(WEBP_DEBUG, "partition_size {}", partition_size);
        sizes = sizes.slice(3);
        if (partition_size > data.size())
            return Error::from_string_literal("WebPImageDecoderPlugin: not enough data for partition data");
        TRY(data_partitions.try_append(data.slice(0, partition_size)));
        data = data.slice(partition_size);
    }
    TRY(data_partitions.try_append(data));
    return data_partitions;
}

}

ErrorOr<NonnullRefPtr<Bitmap>> decode_webp_chunk_VP8_contents(VP8Header const& vp8_header, bool include_alpha_channel)
{
    // The first partition stores header, per-segment state, and macroblock metadata.
    auto decoder = TRY(BooleanDecoder::initialize(vp8_header.first_partition));

    auto header = TRY(decode_VP8_frame_header(decoder));

    // https://datatracker.ietf.org/doc/html/rfc6386#section-2 "Format Overview"
    // "Internally, VP8 decomposes each output frame into an array of
    //  macroblocks.  A macroblock is a square array of pixels whose Y
    //  dimensions are 16x16 and whose U and V dimensions are 8x8."
    int macroblock_width = ceil_div(vp8_header.width, 16);
    int macroblock_height = ceil_div(vp8_header.height, 16);

    auto macroblock_metadata = TRY(decode_VP8_macroblock_metadata(decoder, header, macroblock_width, macroblock_height));

    TRY(decoder.finish_decode());
    // Done with the first partition!

    auto bitmap_format = include_alpha_channel ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
    auto bitmap = TRY(Bitmap::create(bitmap_format, { macroblock_width * 16, macroblock_height * 16 }));

    auto data_partitions = TRY(split_data_partitions(vp8_header.second_partition, header.number_of_dct_partitions));
    TRY(decode_VP8_image_data(*bitmap, header, move(data_partitions), macroblock_width, macroblock_height, macroblock_metadata));

    auto width = static_cast<int>(vp8_header.width);
    auto height = static_cast<int>(vp8_header.height);
    if (bitmap->size() == IntSize { width, height })
        return bitmap;
    return bitmap->cropped({ 0, 0, width, height });
}

}
