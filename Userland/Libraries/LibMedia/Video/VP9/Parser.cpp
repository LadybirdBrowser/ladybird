/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <LibThreading/WorkerThread.h>

#include "Context.h"
#include "Decoder.h"
#include "Parser.h"
#include "Utilities.h"

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

// Beware, threading is unstable in Serenity with smp=on, and performs worse than with it off.
#define VP9_TILE_THREADING

namespace Media::Video::VP9 {

#define TRY_READ(expression) DECODER_TRY(DecoderErrorCategory::Corrupted, expression)

Parser::Parser(Decoder& decoder)
    : m_decoder(decoder)
{
}

Parser::~Parser()
{
}

Vector<size_t> Parser::parse_superframe_sizes(ReadonlyBytes frame_data)
{
    if (frame_data.size() < 1)
        return {};

    // The decoder determines the presence of a superframe by:
    // 1. parsing the final byte of the chunk and checking that the superframe_marker equals 0b110,

    // If the checks in steps 1 and 3 both pass, then the chunk is determined to contain a superframe and each
    // frame in the superframe is passed to the decoding process in turn.
    // Otherwise, the chunk is determined to not contain a superframe, and the whole chunk is passed to the
    // decoding process.

    // NOTE: Reading from span data will be quicker than spinning up a BitStream.
    u8 superframe_byte = frame_data[frame_data.size() - 1];

    // NOTE: We have to read out of the byte from the little end first, hence the padding bits in the masks below.
    u8 superframe_marker = superframe_byte & 0b1110'0000;
    if (superframe_marker == 0b1100'0000) {
        u8 bytes_per_framesize = ((superframe_byte >> 3) & 0b11) + 1;
        u8 frames_in_superframe = (superframe_byte & 0b111) + 1;
        // 2. setting the total size of the superframe_index SzIndex equal to 2 + NumFrames * SzBytes,
        size_t index_size = 2 + bytes_per_framesize * frames_in_superframe;

        if (index_size > frame_data.size())
            return {};

        auto superframe_header_data = frame_data.data() + frame_data.size() - index_size;

        u8 start_superframe_byte = *(superframe_header_data++);
        // 3. checking that the first byte of the superframe_index matches the final byte.
        if (superframe_byte != start_superframe_byte)
            return {};

        Vector<size_t> result;
        for (u8 i = 0; i < frames_in_superframe; i++) {
            size_t frame_size = 0;
            for (u8 j = 0; j < bytes_per_framesize; j++)
                frame_size |= (static_cast<size_t>(*(superframe_header_data++)) << (j * 8));
            result.append(frame_size);
        }
        return result;
    }

    return {};
}

/* (6.1) */
DecoderErrorOr<FrameContext> Parser::parse_frame(ReadonlyBytes frame_data)
{
    if (!m_probability_tables)
        m_probability_tables = DECODER_TRY_ALLOC(try_make<ProbabilityTables>());

    // NOTE: m_reusable_frame_block_contexts does not need to retain any data between frame decodes.
    //       This is only stored so that we don't need to allocate a frame's block contexts on each
    //       call to this function, since it will rarely change sizes.
    auto frame_context = DECODER_TRY_ALLOC(FrameContext::create(frame_data, m_reusable_frame_block_contexts));
    TRY(uncompressed_header(frame_context));
    // FIXME: This should not be an error. Spec says that we consume padding bits until the end of the sample.
    if (frame_context.header_size_in_bytes == 0)
        return DecoderError::corrupted("Frame header is zero-sized"sv);
    m_probability_tables->load_probs(frame_context.probability_context_index);
    m_probability_tables->load_probs2(frame_context.probability_context_index);

    TRY(compressed_header(frame_context));

    TRY(m_decoder.allocate_buffers(frame_context));

    TRY(decode_tiles(frame_context));
    TRY(refresh_probs(frame_context));

    m_previous_frame_type = frame_context.type;
    m_previous_frame_size = frame_context.size();
    m_previous_show_frame = frame_context.shows_a_frame();
    m_previous_color_config = frame_context.color_config;
    m_previous_loop_filter_ref_deltas = frame_context.loop_filter_reference_deltas;
    m_previous_loop_filter_mode_deltas = frame_context.loop_filter_mode_deltas;

    if (frame_context.segmentation_enabled) {
        m_previous_should_use_absolute_segment_base_quantizer = frame_context.should_use_absolute_segment_base_quantizer;
        m_previous_segmentation_features = frame_context.segmentation_features;
    }

    return frame_context;
}

DecoderErrorOr<void> Parser::refresh_probs(FrameContext const& frame_context)
{
    if (!frame_context.error_resilient_mode && !frame_context.parallel_decoding_mode) {
        m_probability_tables->load_probs(frame_context.probability_context_index);
        TRY(m_decoder.adapt_coef_probs(frame_context));
        if (frame_context.is_inter_predicted()) {
            m_probability_tables->load_probs2(frame_context.probability_context_index);
            TRY(m_decoder.adapt_non_coef_probs(frame_context));
        }
    }
    if (frame_context.should_replace_probability_context)
        m_probability_tables->save_probs(frame_context.probability_context_index);
    return {};
}

DecoderErrorOr<VideoFullRangeFlag> Parser::read_video_full_range_flag(BigEndianInputBitStream& bit_stream)
{
    if (TRY_READ(bit_stream.read_bit()))
        return VideoFullRangeFlag::Full;
    return VideoFullRangeFlag::Studio;
}

template<Signed T = i8>
static ErrorOr<T> read_signed(BigEndianInputBitStream& bit_stream, u8 bits)
{
    auto value_unsigned = static_cast<T>(TRY(bit_stream.read_bits(bits)));
    if (TRY(bit_stream.read_bit()))
        return -value_unsigned;
    return value_unsigned;
}

static DecoderErrorOr<i8> read_delta_q(BigEndianInputBitStream& bit_stream)
{
    if (TRY_READ(bit_stream.read_bit()))
        return TRY_READ(read_signed(bit_stream, 4));
    return 0;
}

struct QuantizationParameters {
    u8 base_quantizer_index { 0 };
    i8 y_dc_quantizer_index_delta { 0 };
    i8 uv_dc_quantizer_index_delta { 0 };
    i8 uv_ac_quantizer_index_delta { 0 };
};

static DecoderErrorOr<QuantizationParameters> quantization_params(BigEndianInputBitStream& bit_stream)
{
    QuantizationParameters result;
    result.base_quantizer_index = TRY_READ(bit_stream.read_bits(8));
    result.y_dc_quantizer_index_delta = TRY(read_delta_q(bit_stream));
    result.uv_dc_quantizer_index_delta = TRY(read_delta_q(bit_stream));
    result.uv_ac_quantizer_index_delta = TRY(read_delta_q(bit_stream));
    return result;
}

/* (6.2) */
DecoderErrorOr<void> Parser::uncompressed_header(FrameContext& frame_context)
{
    frame_context.color_config = m_previous_color_config;

    auto frame_marker = TRY_READ(frame_context.bit_stream.read_bits(2));
    if (frame_marker != 2)
        return DecoderError::corrupted("uncompressed_header: Frame marker must be 2"sv);

    auto profile_low_bit = TRY_READ(frame_context.bit_stream.read_bit());
    auto profile_high_bit = TRY_READ(frame_context.bit_stream.read_bit());
    frame_context.profile = (profile_high_bit << 1u) + profile_low_bit;
    if (frame_context.profile == 3 && TRY_READ(frame_context.bit_stream.read_bit()))
        return DecoderError::corrupted("uncompressed_header: Profile 3 reserved bit was non-zero"sv);

    if (TRY_READ(frame_context.bit_stream.read_bit())) {
        frame_context.set_existing_frame_to_show(TRY_READ(frame_context.bit_stream.read_bits(3)));
        return {};
    }

    bool is_keyframe = !TRY_READ(frame_context.bit_stream.read_bit());

    if (!TRY_READ(frame_context.bit_stream.read_bit()))
        frame_context.set_frame_hidden();

    frame_context.error_resilient_mode = TRY_READ(frame_context.bit_stream.read_bit());

    FrameType type;

    Gfx::Size<u32> frame_size;
    Gfx::Size<u32> render_size;
    u8 reference_frames_to_update_flags = 0xFF; // Save frame to all reference indices by default.

    enum class ResetProbabilities : u8 {
        No = 0,
        // 1 also means No here, but we don't need to do anything with the No case.
        OnlyCurrent = 2,
        All = 3,
    };
    ResetProbabilities reset_frame_context = ResetProbabilities::All;

    if (is_keyframe) {
        type = FrameType::KeyFrame;
        TRY(frame_sync_code(frame_context.bit_stream));
        frame_context.color_config = TRY(parse_color_config(frame_context.bit_stream, frame_context.profile));
        frame_size = TRY(parse_frame_size(frame_context.bit_stream));
        render_size = TRY(parse_render_size(frame_context.bit_stream, frame_size));
    } else {
        if (!frame_context.shows_a_frame() && TRY_READ(frame_context.bit_stream.read_bit())) {
            type = FrameType::IntraOnlyFrame;
        } else {
            type = FrameType::InterFrame;
            reset_frame_context = ResetProbabilities::No;
        }

        if (!frame_context.error_resilient_mode)
            reset_frame_context = static_cast<ResetProbabilities>(TRY_READ(frame_context.bit_stream.read_bits(2)));

        if (type == FrameType::IntraOnlyFrame) {
            TRY(frame_sync_code(frame_context.bit_stream));

            if (frame_context.profile == 0) {
                frame_context.color_config = ColorConfig();
            } else {
                frame_context.color_config = TRY(parse_color_config(frame_context.bit_stream, frame_context.profile));
            }

            reference_frames_to_update_flags = TRY_READ(frame_context.bit_stream.read_bits(8));
            frame_size = TRY(parse_frame_size(frame_context.bit_stream));
            render_size = TRY(parse_render_size(frame_context.bit_stream, frame_size));
        } else {
            reference_frames_to_update_flags = TRY_READ(frame_context.bit_stream.read_bits(NUM_REF_FRAMES));
            for (auto i = 0; i < REFS_PER_FRAME; i++) {
                frame_context.reference_frame_indices[i] = TRY_READ(frame_context.bit_stream.read_bits(LOG2_OF_NUM_REF_FRAMES));
                frame_context.reference_frame_sign_biases[ReferenceFrameType::LastFrame + i] = TRY_READ(frame_context.bit_stream.read_bit());
            }
            frame_size = TRY(parse_frame_size_with_refs(frame_context.bit_stream, frame_context.reference_frame_indices));
            render_size = TRY(parse_render_size(frame_context.bit_stream, frame_size));
            frame_context.high_precision_motion_vectors_allowed = TRY_READ(frame_context.bit_stream.read_bit());
            frame_context.interpolation_filter = TRY(read_interpolation_filter(frame_context.bit_stream));
            for (auto i = 0; i < REFS_PER_FRAME; i++) {
                TRY(m_decoder.prepare_referenced_frame(frame_size, frame_context.reference_frame_indices[i]));
            }
        }
    }

    bool should_replace_probability_context = false;
    bool parallel_decoding_mode = true;
    if (!frame_context.error_resilient_mode) {
        should_replace_probability_context = TRY_READ(frame_context.bit_stream.read_bit());
        parallel_decoding_mode = TRY_READ(frame_context.bit_stream.read_bit());
    }

    u8 probability_context_index = TRY_READ(frame_context.bit_stream.read_bits(2));
    switch (reset_frame_context) {
    case ResetProbabilities::All:
        setup_past_independence();
        for (auto i = 0; i < 4; i++) {
            m_probability_tables->save_probs(i);
        }
        probability_context_index = 0;
        break;
    case ResetProbabilities::OnlyCurrent:
        setup_past_independence();
        m_probability_tables->save_probs(probability_context_index);
        probability_context_index = 0;
        break;
    default:
        break;
    }

    frame_context.type = type;
    DECODER_TRY_ALLOC(frame_context.set_size(frame_size));
    frame_context.render_size = render_size;
    TRY(compute_image_size(frame_context));

    frame_context.reference_frames_to_update_flags = reference_frames_to_update_flags;
    frame_context.parallel_decoding_mode = parallel_decoding_mode;

    frame_context.should_replace_probability_context = should_replace_probability_context;
    frame_context.probability_context_index = probability_context_index;

    TRY(loop_filter_params(frame_context));
    auto quantization_parameters = TRY(quantization_params(frame_context.bit_stream));
    TRY(segmentation_params(frame_context));
    precalculate_quantizers(frame_context, quantization_parameters);

    TRY(parse_tile_counts(frame_context));

    frame_context.header_size_in_bytes = TRY_READ(frame_context.bit_stream.read_bits(16));

    frame_context.bit_stream.align_to_byte_boundary();
    return {};
}

DecoderErrorOr<void> Parser::frame_sync_code(BigEndianInputBitStream& bit_stream)
{
    if (TRY_READ(bit_stream.read_bits(24)) != 0x498342) {
        return DecoderError::corrupted("frame sync code was not 0x498342."sv);
    }
    return {};
}

DecoderErrorOr<ColorConfig> Parser::parse_color_config(BigEndianInputBitStream& bit_stream, u8 profile)
{
    // (6.2.2) color_config( )
    u8 bit_depth;
    if (profile >= 2) {
        bit_depth = TRY_READ(bit_stream.read_bit()) ? 12 : 10;
    } else {
        bit_depth = 8;
    }

    auto color_space = static_cast<ColorSpace>(TRY_READ(bit_stream.read_bits(3)));
    if (color_space == ColorSpace::Reserved)
        return DecoderError::corrupted("color_config: Color space reserved value was set"sv);

    VERIFY(color_space <= ColorSpace::RGB);

    VideoFullRangeFlag video_full_range_flag;
    bool subsampling_x, subsampling_y;

    if (color_space != ColorSpace::RGB) {
        video_full_range_flag = TRY(read_video_full_range_flag(bit_stream));
        if (profile == 1 || profile == 3) {
            subsampling_x = TRY_READ(bit_stream.read_bit());
            subsampling_y = TRY_READ(bit_stream.read_bit());
            if (TRY_READ(bit_stream.read_bit()))
                return DecoderError::corrupted("color_config: Subsampling reserved zero was set"sv);
        } else {
            subsampling_x = true;
            subsampling_y = true;
        }
    } else {
        video_full_range_flag = VideoFullRangeFlag::Full;
        if (profile == 1 || profile == 3) {
            subsampling_x = false;
            subsampling_y = false;
            if (TRY_READ(bit_stream.read_bit()))
                return DecoderError::corrupted("color_config: RGB reserved zero was set"sv);
        } else {
            // FIXME: Spec does not specify the subsampling value here. Is this an error or should we set a default?
            return DecoderError::corrupted("color_config: Invalid subsampling value for profile 0 or 2"sv);
        }
    }

    return ColorConfig { bit_depth, color_space, video_full_range_flag, subsampling_x, subsampling_y };
}

DecoderErrorOr<Gfx::Size<u32>> Parser::parse_frame_size(BigEndianInputBitStream& bit_stream)
{
    return Gfx::Size<u32> { TRY_READ(bit_stream.read_bits(16)) + 1, TRY_READ(bit_stream.read_bits(16)) + 1 };
}

DecoderErrorOr<Gfx::Size<u32>> Parser::parse_render_size(BigEndianInputBitStream& bit_stream, Gfx::Size<u32> frame_size)
{
    // FIXME: This function should save this bit as a value in the FrameContext. The bit can be
    //        used in files where the pixel aspect ratio changes between samples in the video.
    //        If the bit is set, the pixel aspect ratio should be recalculated, whereas if only
    //        the frame size has changed and the render size is unadjusted, then the pixel aspect
    //        ratio should be retained and the new render size determined based on that.
    //        See the Firefox source code here:
    //        https://searchfox.org/mozilla-central/source/dom/media/platforms/wrappers/MediaChangeMonitor.cpp#268-276
    if (!TRY_READ(bit_stream.read_bit()))
        return frame_size;
    return Gfx::Size<u32> { TRY_READ(bit_stream.read_bits(16)) + 1, TRY_READ(bit_stream.read_bits(16)) + 1 };
}

DecoderErrorOr<Gfx::Size<u32>> Parser::parse_frame_size_with_refs(BigEndianInputBitStream& bit_stream, Array<u8, 3> const& reference_indices)
{
    Optional<Gfx::Size<u32>> size;
    for (auto frame_index : reference_indices) {
        if (TRY_READ(bit_stream.read_bit())) {
            if (!m_reference_frames[frame_index].is_valid())
                return DecoderError::corrupted("Frame size referenced a frame that does not exist"sv);
            size.emplace(m_reference_frames[frame_index].size);
            break;
        }
    }

    if (size.has_value())
        return size.value();

    return TRY(parse_frame_size(bit_stream));
}

DecoderErrorOr<void> Parser::compute_image_size(FrameContext& frame_context)
{
    // 7.2.6 Compute image size semantics
    // When compute_image_size is invoked, the following ordered steps occur:
    // 1. If this is the first time compute_image_size is invoked, or if either FrameWidth or FrameHeight have
    // changed in value compared to the previous time this function was invoked, then the segmentation map is
    // cleared to all zeros by setting SegmentId[ row ][ col ] equal to 0 for row = 0..MiRows-1 and col =
    // 0..MiCols-1.
    // FIXME: What does this mean? SegmentIds is scoped to one frame, so it will not contain values here. It's
    //        also suspicious that spec refers to this as SegmentId rather than SegmentIds (plural). Is this
    //        supposed to refer to PrevSegmentIds?
    bool first_invoke = m_is_first_compute_image_size_invoke;
    m_is_first_compute_image_size_invoke = false;
    bool same_size = m_previous_frame_size == frame_context.size();

    // 2. The variable UsePrevFrameMvs is set equal to 1 if all of the following conditions are true:
    // a. This is not the first time compute_image_size is invoked.
    // b. Both FrameWidth and FrameHeight have the same value compared to the previous time this function
    // was invoked.
    // c. show_frame was equal to 1 the previous time this function was invoked.
    // d. error_resilient_mode is equal to 0.
    // e. FrameIsIntra is equal to 0.
    // Otherwise, UsePrevFrameMvs is set equal to 0.
    frame_context.use_previous_frame_motion_vectors = !first_invoke && same_size && m_previous_show_frame && !frame_context.error_resilient_mode && frame_context.is_inter_predicted();
    return {};
}

DecoderErrorOr<InterpolationFilter> Parser::read_interpolation_filter(BigEndianInputBitStream& bit_stream)
{
    if (TRY_READ(bit_stream.read_bit())) {
        return InterpolationFilter::Switchable;
    }
    return literal_to_type[TRY_READ(bit_stream.read_bits(2))];
}

DecoderErrorOr<void> Parser::loop_filter_params(FrameContext& frame_context)
{
    // FIXME: These should be moved to their own struct to return here.
    frame_context.loop_filter_level = TRY_READ(frame_context.bit_stream.read_bits(6));
    frame_context.loop_filter_sharpness = TRY_READ(frame_context.bit_stream.read_bits(3));
    frame_context.loop_filter_delta_enabled = TRY_READ(frame_context.bit_stream.read_bit());

    auto reference_deltas = m_previous_loop_filter_ref_deltas;
    auto mode_deltas = m_previous_loop_filter_mode_deltas;
    if (frame_context.loop_filter_delta_enabled && TRY_READ(frame_context.bit_stream.read_bit())) {
        for (auto& loop_filter_ref_delta : reference_deltas) {
            if (TRY_READ(frame_context.bit_stream.read_bit()))
                loop_filter_ref_delta = TRY_READ(read_signed(frame_context.bit_stream, 6));
        }
        for (auto& loop_filter_mode_delta : mode_deltas) {
            if (TRY_READ(frame_context.bit_stream.read_bit()))
                loop_filter_mode_delta = TRY_READ(read_signed(frame_context.bit_stream, 6));
        }
    }
    frame_context.loop_filter_reference_deltas = reference_deltas;
    frame_context.loop_filter_mode_deltas = mode_deltas;

    return {};
}

DecoderErrorOr<void> Parser::segmentation_params(FrameContext& frame_context)
{
    frame_context.segmentation_enabled = TRY_READ(frame_context.bit_stream.read_bit());
    if (!frame_context.segmentation_enabled)
        return {};

    frame_context.should_use_absolute_segment_base_quantizer = m_previous_should_use_absolute_segment_base_quantizer;
    frame_context.segmentation_features = m_previous_segmentation_features;

    if (TRY_READ(frame_context.bit_stream.read_bit())) {
        frame_context.use_full_segment_id_tree = true;
        for (auto& segmentation_tree_prob : frame_context.full_segment_id_tree_probabilities)
            segmentation_tree_prob = TRY(read_prob(frame_context.bit_stream));

        if (TRY_READ(frame_context.bit_stream.read_bit())) {
            frame_context.use_predicted_segment_id_tree = true;
            for (auto& segmentation_pred_prob : frame_context.predicted_segment_id_tree_probabilities)
                segmentation_pred_prob = TRY(read_prob(frame_context.bit_stream));
        }
    }

    auto segmentation_update_data = (TRY_READ(frame_context.bit_stream.read_bit()));

    if (!segmentation_update_data)
        return {};

    frame_context.should_use_absolute_segment_base_quantizer = TRY_READ(frame_context.bit_stream.read_bit());
    for (auto segment_id = 0; segment_id < MAX_SEGMENTS; segment_id++) {
        for (auto feature_id = 0; feature_id < to_underlying(SegmentFeature::Sentinel); feature_id++) {
            auto& feature = frame_context.segmentation_features[segment_id][feature_id];
            feature.enabled = TRY_READ(frame_context.bit_stream.read_bit());
            if (feature.enabled) {
                auto bits_to_read = segmentation_feature_bits[feature_id];
                feature.value = TRY_READ(frame_context.bit_stream.read_bits(bits_to_read));
                if (segmentation_feature_signed[feature_id]) {
                    if (TRY_READ(frame_context.bit_stream.read_bit()))
                        feature.value = -feature.value;
                }
            }
        }
    }

    return {};
}

DecoderErrorOr<u8> Parser::read_prob(BigEndianInputBitStream& bit_stream)
{
    if (TRY_READ(bit_stream.read_bit()))
        return TRY_READ(bit_stream.read_bits(8));
    return 255;
}

void Parser::precalculate_quantizers(FrameContext& frame_context, QuantizationParameters quantization_parameters)
{
    frame_context.lossless = quantization_parameters.base_quantizer_index == 0
        && quantization_parameters.y_dc_quantizer_index_delta == 0
        && quantization_parameters.uv_dc_quantizer_index_delta == 0
        && quantization_parameters.uv_ac_quantizer_index_delta == 0;

    // Pre-calculate the quantizers so that the decoder doesn't have to do it repeatedly.
    for (u8 segment_id = 0; segment_id < MAX_SEGMENTS; segment_id++) {
        auto alternative_quantizer_feature = frame_context.get_segment_feature(segment_id, SegmentFeature::AlternativeQuantizerBase);
        auto base = Decoder::get_base_quantizer_index(alternative_quantizer_feature, frame_context.should_use_absolute_segment_base_quantizer, quantization_parameters.base_quantizer_index);

        // The function get_ac_quant( plane ) returns the quantizer value for the ac coefficient for a particular plane and
        // is derived as follows:
        // − If plane is equal to 0, return ac_q( get_qindex( ) ).
        // − Otherwise, return ac_q( get_qindex( ) + delta_q_uv_ac ).
        auto& current_quantizers = frame_context.segment_quantizers[segment_id];
        current_quantizers.y_ac_quantizer = Decoder::get_ac_quantizer(frame_context.color_config.bit_depth, base, 0);
        current_quantizers.uv_ac_quantizer = Decoder::get_ac_quantizer(frame_context.color_config.bit_depth, base, quantization_parameters.uv_ac_quantizer_index_delta);

        // The function get_dc_quant( plane ) returns the quantizer value for the dc coefficient for a particular plane and
        // is derived as follows:
        // − If plane is equal to 0, return dc_q( get_qindex( ) + delta_q_y_dc ).
        // − Otherwise, return dc_q( get_qindex( ) + delta_q_uv_dc ).
        current_quantizers.y_dc_quantizer = Decoder::get_dc_quantizer(frame_context.color_config.bit_depth, base, quantization_parameters.y_dc_quantizer_index_delta);
        current_quantizers.uv_dc_quantizer = Decoder::get_dc_quantizer(frame_context.color_config.bit_depth, base, quantization_parameters.uv_dc_quantizer_index_delta);
    }
}

static u16 calc_min_log2_of_tile_columns(u32 superblock_columns)
{
    auto min_log_2 = 0u;
    while ((u32)(MAX_TILE_WIDTH_B64 << min_log_2) < superblock_columns)
        min_log_2++;
    return min_log_2;
}

static u16 calc_max_log2_tile_cols(u32 superblock_columns)
{
    u16 max_log_2 = 1;
    while ((superblock_columns >> max_log_2) >= MIN_TILE_WIDTH_B64)
        max_log_2++;
    return max_log_2 - 1;
}

DecoderErrorOr<void> Parser::parse_tile_counts(FrameContext& frame_context)
{
    auto superblock_columns = frame_context.superblock_columns();

    auto log2_of_tile_columns = calc_min_log2_of_tile_columns(superblock_columns);
    auto log2_of_tile_columns_maximum = calc_max_log2_tile_cols(superblock_columns);
    while (log2_of_tile_columns < log2_of_tile_columns_maximum) {
        if (TRY_READ(frame_context.bit_stream.read_bit()))
            log2_of_tile_columns++;
        else
            break;
    }

    u16 log2_of_tile_rows = TRY_READ(frame_context.bit_stream.read_bit());
    if (log2_of_tile_rows > 0) {
        log2_of_tile_rows += TRY_READ(frame_context.bit_stream.read_bit());
    }
    frame_context.log2_of_tile_counts = Gfx::Size<u16>(log2_of_tile_columns, log2_of_tile_rows);
    return {};
}

void Parser::setup_past_independence()
{
    m_previous_block_contexts.reset();
    m_previous_loop_filter_ref_deltas[ReferenceFrameType::None] = 1;
    m_previous_loop_filter_ref_deltas[ReferenceFrameType::LastFrame] = 0;
    m_previous_loop_filter_ref_deltas[ReferenceFrameType::GoldenFrame] = -1;
    m_previous_loop_filter_ref_deltas[ReferenceFrameType::AltRefFrame] = -1;
    m_previous_loop_filter_mode_deltas.fill(0);
    m_previous_should_use_absolute_segment_base_quantizer = false;
    for (auto& segment_levels : m_previous_segmentation_features)
        segment_levels.fill({ false, 0 });
    m_probability_tables->reset_probs();
}

DecoderErrorOr<void> Parser::compressed_header(FrameContext& frame_context)
{
    auto decoder = TRY(frame_context.create_range_decoder(frame_context.header_size_in_bytes));

    frame_context.transform_mode = read_tx_mode(decoder, frame_context);
    if (frame_context.transform_mode == TransformMode::Select)
        tx_mode_probs(decoder);
    read_coef_probs(decoder, frame_context.transform_mode);
    read_skip_prob(decoder);
    if (frame_context.is_inter_predicted()) {
        read_inter_mode_probs(decoder);
        if (frame_context.interpolation_filter == Switchable)
            read_interp_filter_probs(decoder);
        read_is_inter_probs(decoder);
        frame_reference_mode(frame_context, decoder);
        frame_reference_mode_probs(decoder, frame_context);
        read_y_mode_probs(decoder);
        read_partition_probs(decoder);
        mv_probs(decoder, frame_context);
    }
    TRY_READ(decoder.finish_decode());
    return {};
}

TransformMode Parser::read_tx_mode(BooleanDecoder& decoder, FrameContext const& frame_context)
{
    if (frame_context.lossless) {
        return TransformMode::Only_4x4;
    }

    auto tx_mode = decoder.read_literal(2);
    if (tx_mode == to_underlying(TransformMode::Allow_32x32))
        tx_mode += decoder.read_literal(1);
    return static_cast<TransformMode>(tx_mode);
}

void Parser::tx_mode_probs(BooleanDecoder& decoder)
{
    auto& tx_probs = m_probability_tables->tx_probs();
    for (auto i = 0; i < TX_SIZE_CONTEXTS; i++) {
        for (auto j = 0; j < TX_SIZES - 3; j++)
            tx_probs[Transform_8x8][i][j] = diff_update_prob(decoder, tx_probs[Transform_8x8][i][j]);
    }
    for (auto i = 0; i < TX_SIZE_CONTEXTS; i++) {
        for (auto j = 0; j < TX_SIZES - 2; j++)
            tx_probs[Transform_16x16][i][j] = diff_update_prob(decoder, tx_probs[Transform_16x16][i][j]);
    }
    for (auto i = 0; i < TX_SIZE_CONTEXTS; i++) {
        for (auto j = 0; j < TX_SIZES - 1; j++)
            tx_probs[Transform_32x32][i][j] = diff_update_prob(decoder, tx_probs[Transform_32x32][i][j]);
    }
}

u8 Parser::diff_update_prob(BooleanDecoder& decoder, u8 prob)
{
    auto update_prob = decoder.read_bool(252);
    if (update_prob) {
        auto delta_prob = decode_term_subexp(decoder);
        prob = inv_remap_prob(delta_prob, prob);
    }
    return prob;
}

u8 Parser::decode_term_subexp(BooleanDecoder& decoder)
{
    if (decoder.read_literal(1) == 0)
        return decoder.read_literal(4);
    if (decoder.read_literal(1) == 0)
        return decoder.read_literal(4) + 16;
    if (decoder.read_literal(1) == 0)
        return decoder.read_literal(5) + 32;

    auto v = decoder.read_literal(7);
    if (v < 65)
        return v + 64;
    return (v << 1u) - 1 + decoder.read_literal(1);
}

u8 Parser::inv_remap_prob(u8 delta_prob, u8 prob)
{
    u8 m = prob - 1;
    auto v = inv_map_table[delta_prob];
    if ((m << 1u) <= 255)
        return 1 + inv_recenter_nonneg(v, m);
    return 255 - inv_recenter_nonneg(v, 254 - m);
}

u8 Parser::inv_recenter_nonneg(u8 v, u8 m)
{
    if (v > 2 * m)
        return v;
    if (v & 1u)
        return m - ((v + 1u) >> 1u);
    return m + (v >> 1u);
}

void Parser::read_coef_probs(BooleanDecoder& decoder, TransformMode transform_mode)
{
    auto max_tx_size = tx_mode_to_biggest_tx_size[to_underlying(transform_mode)];
    for (u8 transform_size = 0; transform_size <= max_tx_size; transform_size++) {
        auto update_probs = decoder.read_literal(1);
        if (update_probs == 1) {
            for (auto i = 0; i < 2; i++) {
                for (auto j = 0; j < 2; j++) {
                    for (auto k = 0; k < 6; k++) {
                        auto max_l = (k == 0) ? 3 : 6;
                        for (auto l = 0; l < max_l; l++) {
                            for (auto m = 0; m < 3; m++) {
                                auto& prob = m_probability_tables->coef_probs()[transform_size][i][j][k][l][m];
                                prob = diff_update_prob(decoder, prob);
                            }
                        }
                    }
                }
            }
        }
    }
}

void Parser::read_skip_prob(BooleanDecoder& decoder)
{
    for (auto i = 0; i < SKIP_CONTEXTS; i++)
        m_probability_tables->skip_prob()[i] = diff_update_prob(decoder, m_probability_tables->skip_prob()[i]);
}

void Parser::read_inter_mode_probs(BooleanDecoder& decoder)
{
    for (auto i = 0; i < INTER_MODE_CONTEXTS; i++) {
        for (auto j = 0; j < INTER_MODES - 1; j++)
            m_probability_tables->inter_mode_probs()[i][j] = diff_update_prob(decoder, m_probability_tables->inter_mode_probs()[i][j]);
    }
}

void Parser::read_interp_filter_probs(BooleanDecoder& decoder)
{
    for (auto i = 0; i < INTERP_FILTER_CONTEXTS; i++) {
        for (auto j = 0; j < SWITCHABLE_FILTERS - 1; j++)
            m_probability_tables->interp_filter_probs()[i][j] = diff_update_prob(decoder, m_probability_tables->interp_filter_probs()[i][j]);
    }
}

void Parser::read_is_inter_probs(BooleanDecoder& decoder)
{
    for (auto i = 0; i < IS_INTER_CONTEXTS; i++)
        m_probability_tables->is_inter_prob()[i] = diff_update_prob(decoder, m_probability_tables->is_inter_prob()[i]);
}

static void setup_compound_reference_mode(FrameContext& frame_context)
{
    ReferenceFrameType fixed_reference;
    ReferenceFramePair variable_references;
    if (frame_context.reference_frame_sign_biases[ReferenceFrameType::LastFrame] == frame_context.reference_frame_sign_biases[ReferenceFrameType::GoldenFrame]) {
        fixed_reference = ReferenceFrameType::AltRefFrame;
        variable_references = { ReferenceFrameType::LastFrame, ReferenceFrameType::GoldenFrame };
    } else if (frame_context.reference_frame_sign_biases[ReferenceFrameType::LastFrame] == frame_context.reference_frame_sign_biases[ReferenceFrameType::AltRefFrame]) {
        fixed_reference = ReferenceFrameType::GoldenFrame;
        variable_references = { ReferenceFrameType::LastFrame, ReferenceFrameType::AltRefFrame };
    } else {
        fixed_reference = ReferenceFrameType::LastFrame;
        variable_references = { ReferenceFrameType::GoldenFrame, ReferenceFrameType::AltRefFrame };
    }
    frame_context.fixed_reference_type = fixed_reference;
    frame_context.variable_reference_types = variable_references;
}

void Parser::frame_reference_mode(FrameContext& frame_context, BooleanDecoder& decoder)
{
    auto compound_reference_allowed = false;
    for (size_t i = 2; i <= REFS_PER_FRAME; i++) {
        if (frame_context.reference_frame_sign_biases[i] != frame_context.reference_frame_sign_biases[1])
            compound_reference_allowed = true;
    }
    ReferenceMode reference_mode;
    if (compound_reference_allowed) {
        auto non_single_reference = decoder.read_literal(1);
        if (non_single_reference == 0) {
            reference_mode = SingleReference;
        } else {
            auto reference_select = decoder.read_literal(1);
            if (reference_select == 0)
                reference_mode = CompoundReference;
            else
                reference_mode = ReferenceModeSelect;
        }
    } else {
        reference_mode = SingleReference;
    }
    frame_context.reference_mode = reference_mode;
    if (reference_mode != SingleReference)
        setup_compound_reference_mode(frame_context);
}

void Parser::frame_reference_mode_probs(BooleanDecoder& decoder, FrameContext const& frame_context)
{
    if (frame_context.reference_mode == ReferenceModeSelect) {
        for (auto i = 0; i < COMP_MODE_CONTEXTS; i++) {
            auto& comp_mode_prob = m_probability_tables->comp_mode_prob();
            comp_mode_prob[i] = diff_update_prob(decoder, comp_mode_prob[i]);
        }
    }
    if (frame_context.reference_mode != CompoundReference) {
        for (auto i = 0; i < REF_CONTEXTS; i++) {
            auto& single_ref_prob = m_probability_tables->single_ref_prob();
            single_ref_prob[i][0] = diff_update_prob(decoder, single_ref_prob[i][0]);
            single_ref_prob[i][1] = diff_update_prob(decoder, single_ref_prob[i][1]);
        }
    }
    if (frame_context.reference_mode != SingleReference) {
        for (auto i = 0; i < REF_CONTEXTS; i++) {
            auto& comp_ref_prob = m_probability_tables->comp_ref_prob();
            comp_ref_prob[i] = diff_update_prob(decoder, comp_ref_prob[i]);
        }
    }
}

void Parser::read_y_mode_probs(BooleanDecoder& decoder)
{
    for (auto i = 0; i < BLOCK_SIZE_GROUPS; i++) {
        for (auto j = 0; j < INTRA_MODES - 1; j++) {
            auto& y_mode_probs = m_probability_tables->y_mode_probs();
            y_mode_probs[i][j] = diff_update_prob(decoder, y_mode_probs[i][j]);
        }
    }
}

void Parser::read_partition_probs(BooleanDecoder& decoder)
{
    for (auto i = 0; i < PARTITION_CONTEXTS; i++) {
        for (auto j = 0; j < PARTITION_TYPES - 1; j++) {
            auto& partition_probs = m_probability_tables->partition_probs();
            partition_probs[i][j] = diff_update_prob(decoder, partition_probs[i][j]);
        }
    }
}

void Parser::mv_probs(BooleanDecoder& decoder, FrameContext const& frame_context)
{
    for (auto j = 0; j < MV_JOINTS - 1; j++) {
        auto& mv_joint_probs = m_probability_tables->mv_joint_probs();
        mv_joint_probs[j] = update_mv_prob(decoder, mv_joint_probs[j]);
    }

    for (auto i = 0; i < 2; i++) {
        auto& mv_sign_prob = m_probability_tables->mv_sign_prob();
        mv_sign_prob[i] = update_mv_prob(decoder, mv_sign_prob[i]);
        for (auto j = 0; j < MV_CLASSES - 1; j++) {
            auto& mv_class_probs = m_probability_tables->mv_class_probs();
            mv_class_probs[i][j] = update_mv_prob(decoder, mv_class_probs[i][j]);
        }
        auto& mv_class0_bit_prob = m_probability_tables->mv_class0_bit_prob();
        mv_class0_bit_prob[i] = update_mv_prob(decoder, mv_class0_bit_prob[i]);
        for (auto j = 0; j < MV_OFFSET_BITS; j++) {
            auto& mv_bits_prob = m_probability_tables->mv_bits_prob();
            mv_bits_prob[i][j] = update_mv_prob(decoder, mv_bits_prob[i][j]);
        }
    }

    for (auto i = 0; i < 2; i++) {
        for (auto j = 0; j < CLASS0_SIZE; j++) {
            for (auto k = 0; k < MV_FR_SIZE - 1; k++) {
                auto& mv_class0_fr_probs = m_probability_tables->mv_class0_fr_probs();
                mv_class0_fr_probs[i][j][k] = update_mv_prob(decoder, mv_class0_fr_probs[i][j][k]);
            }
        }
        for (auto k = 0; k < MV_FR_SIZE - 1; k++) {
            auto& mv_fr_probs = m_probability_tables->mv_fr_probs();
            mv_fr_probs[i][k] = update_mv_prob(decoder, mv_fr_probs[i][k]);
        }
    }

    if (frame_context.high_precision_motion_vectors_allowed) {
        for (auto i = 0; i < 2; i++) {
            auto& mv_class0_hp_prob = m_probability_tables->mv_class0_hp_prob();
            auto& mv_hp_prob = m_probability_tables->mv_hp_prob();
            mv_class0_hp_prob[i] = update_mv_prob(decoder, mv_class0_hp_prob[i]);
            mv_hp_prob[i] = update_mv_prob(decoder, mv_hp_prob[i]);
        }
    }
}

u8 Parser::update_mv_prob(BooleanDecoder& decoder, u8 prob)
{
    if (decoder.read_bool(252)) {
        return (decoder.read_literal(7) << 1u) | 1u;
    }
    return prob;
}

static u32 get_tile_offset(u32 tile_start, u32 frame_size_in_blocks, u32 tile_size_log2)
{
    u32 superblocks = blocks_ceiled_to_superblocks(frame_size_in_blocks);
    u32 offset = superblocks_to_blocks((tile_start * superblocks) >> tile_size_log2);
    return min(offset, frame_size_in_blocks);
}

DecoderErrorOr<void> Parser::decode_tiles(FrameContext& frame_context)
{
    auto log2_dimensions = frame_context.log2_of_tile_counts;
    auto tile_cols = 1u << log2_dimensions.width();
    auto tile_rows = 1u << log2_dimensions.height();

    PartitionContext above_partition_context = DECODER_TRY_ALLOC(PartitionContext::create(superblocks_to_blocks(frame_context.superblock_columns())));
    NonZeroTokens above_non_zero_tokens = DECODER_TRY_ALLOC(create_non_zero_tokens(blocks_to_sub_blocks(frame_context.columns()), frame_context.color_config.subsampling_x));
    SegmentationPredictionContext above_segmentation_ids = DECODER_TRY_ALLOC(SegmentationPredictionContext::create(frame_context.columns()));

    // FIXME: To implement tiled decoding, we'll need to pre-parse the tile positions and sizes into a 2D vector of ReadonlyBytes,
    //        then run through each column of tiles in top to bottom order afterward. Each column can be sent to a worker thread
    //        for execution. Each worker thread will want to create a set of above contexts sized to its tile width, then provide
    //        those to each tile as it decodes them.
    Vector<Vector<TileContext, 1>, 4> tile_workloads;
    DECODER_TRY_ALLOC(tile_workloads.try_ensure_capacity(tile_cols));
    for (auto tile_col = 0u; tile_col < tile_cols; tile_col++) {
        tile_workloads.append({});
        DECODER_TRY_ALLOC(tile_workloads[tile_col].try_ensure_capacity(tile_rows));
    }

    for (auto tile_row = 0u; tile_row < tile_rows; tile_row++) {
        for (auto tile_col = 0u; tile_col < tile_cols; tile_col++) {
            auto last_tile = (tile_row == tile_rows - 1) && (tile_col == tile_cols - 1);
            size_t tile_size;
            if (last_tile)
                tile_size = frame_context.stream->remaining();
            else
                tile_size = TRY_READ(frame_context.bit_stream.read_bits(32));

            auto rows_start = get_tile_offset(tile_row, frame_context.rows(), log2_dimensions.height());
            auto rows_end = get_tile_offset(tile_row + 1, frame_context.rows(), log2_dimensions.height());
            auto columns_start = get_tile_offset(tile_col, frame_context.columns(), log2_dimensions.width());
            auto columns_end = get_tile_offset(tile_col + 1, frame_context.columns(), log2_dimensions.width());

            auto width = columns_end - columns_start;
            auto above_partition_context_for_tile = above_partition_context.span().slice(columns_start, superblocks_to_blocks(blocks_ceiled_to_superblocks(width)));
            auto above_non_zero_tokens_view = create_non_zero_tokens_view(above_non_zero_tokens, blocks_to_sub_blocks(columns_start), blocks_to_sub_blocks(columns_end - columns_start), frame_context.color_config.subsampling_x);
            auto above_segmentation_ids_for_tile = safe_slice(above_segmentation_ids.span(), columns_start, columns_end - columns_start);

            tile_workloads[tile_col].append(TRY(TileContext::try_create(frame_context, tile_size, rows_start, rows_end, columns_start, columns_end, above_partition_context_for_tile, above_non_zero_tokens_view, above_segmentation_ids_for_tile)));
        }
    }

    auto decode_tile_column = [this, tile_rows](auto& column_workloads) -> DecoderErrorOr<void> {
        VERIFY(column_workloads.size() == tile_rows);
        for (auto tile_row = 0u; tile_row < tile_rows; tile_row++)
            TRY(decode_tile(column_workloads[tile_row]));
        return {};
    };

#ifdef VP9_TILE_THREADING
    auto const worker_count = tile_cols - 1;

    if (m_worker_threads.size() < worker_count) {
        m_worker_threads.clear();
        m_worker_threads.ensure_capacity(worker_count);
        for (auto i = 0u; i < worker_count; i++)
            m_worker_threads.append(DECODER_TRY_ALLOC(Threading::WorkerThread<DecoderError>::create("Decoder Worker"sv)));
    }
    VERIFY(m_worker_threads.size() >= worker_count);

    // Start tile column decoding tasks in thread workers starting from the second column.
    for (auto tile_col = 1u; tile_col < tile_cols; tile_col++) {
        auto& column_workload = tile_workloads[tile_col];
        m_worker_threads[tile_col - 1]->start_task([&decode_tile_column, &column_workload]() -> DecoderErrorOr<void> {
            return decode_tile_column(column_workload);
        });
    }

    // Decode the first column in this thread.
    auto result = decode_tile_column(tile_workloads[0]);

    for (auto& worker_thread : m_worker_threads) {
        auto task_result = worker_thread->wait_until_task_is_finished();
        if (!result.is_error() && task_result.is_error())
            result = move(task_result);
    }

    if (result.is_error())
        return result;
#else
    for (auto& column_workloads : tile_workloads)
        TRY(decode_tile_column(column_workloads));
#endif

    // Sum up all tile contexts' syntax element counters after all decodes have finished.
    for (auto& tile_contexts : tile_workloads) {
        for (auto& tile_context : tile_contexts) {
            *frame_context.counter += *tile_context.counter;
        }
    }

    return {};
}

DecoderErrorOr<void> Parser::decode_tile(TileContext& tile_context)
{
    for (auto row = tile_context.rows_start; row < tile_context.rows_end; row += 8) {
        clear_left_context(tile_context);
        for (auto col = tile_context.columns_start; col < tile_context.columns_end; col += 8) {
            TRY(decode_partition(tile_context, row, col, Block_64x64));
        }
    }
    TRY_READ(tile_context.decoder.finish_decode());
    return {};
}

void Parser::clear_left_context(TileContext& tile_context)
{
    for (auto& context_for_plane : tile_context.left_non_zero_tokens)
        context_for_plane.fill_with(false);
    tile_context.left_segmentation_ids.fill_with(0);
    tile_context.left_partition_context.fill_with(0);
}

DecoderErrorOr<void> Parser::decode_partition(TileContext& tile_context, u32 row, u32 column, BlockSubsize subsize)
{
    if (row >= tile_context.frame_context.rows() || column >= tile_context.frame_context.columns())
        return {};
    u8 num_8x8 = num_8x8_blocks_wide_lookup[subsize];
    auto half_block_8x8 = num_8x8 >> 1;
    bool has_rows = (row + half_block_8x8) < tile_context.frame_context.rows();
    bool has_cols = (column + half_block_8x8) < tile_context.frame_context.columns();
    u32 row_in_tile = row - tile_context.rows_start;
    u32 column_in_tile = column - tile_context.columns_start;
    auto partition = TreeParser::parse_partition(tile_context.decoder, *m_probability_tables, *tile_context.counter, has_rows, has_cols, subsize, num_8x8, tile_context.above_partition_context, tile_context.left_partition_context.span(), row_in_tile, column_in_tile, !tile_context.frame_context.is_inter_predicted());

    auto child_subsize = subsize_lookup[partition][subsize];
    if (child_subsize < Block_8x8 || partition == PartitionNone) {
        TRY(decode_block(tile_context, row, column, child_subsize));
    } else if (partition == PartitionHorizontal) {
        TRY(decode_block(tile_context, row, column, child_subsize));
        if (has_rows)
            TRY(decode_block(tile_context, row + half_block_8x8, column, child_subsize));
    } else if (partition == PartitionVertical) {
        TRY(decode_block(tile_context, row, column, child_subsize));
        if (has_cols)
            TRY(decode_block(tile_context, row, column + half_block_8x8, child_subsize));
    } else {
        TRY(decode_partition(tile_context, row, column, child_subsize));
        TRY(decode_partition(tile_context, row, column + half_block_8x8, child_subsize));
        TRY(decode_partition(tile_context, row + half_block_8x8, column, child_subsize));
        TRY(decode_partition(tile_context, row + half_block_8x8, column + half_block_8x8, child_subsize));
    }
    if (subsize == Block_8x8 || partition != PartitionSplit) {
        auto above_context = 15 >> b_width_log2_lookup[child_subsize];
        auto left_context = 15 >> b_height_log2_lookup[child_subsize];
        for (size_t i = 0; i < num_8x8; i++) {
            tile_context.above_partition_context[column_in_tile + i] = above_context;
            tile_context.left_partition_context[row_in_tile + i] = left_context;
        }
    }
    return {};
}

size_t Parser::get_image_index(FrameContext const& frame_context, u32 row, u32 column) const
{
    VERIFY(row < frame_context.rows() && column < frame_context.columns());
    return row * frame_context.columns() + column;
}

DecoderErrorOr<void> Parser::decode_block(TileContext& tile_context, u32 row, u32 column, BlockSubsize subsize)
{
    auto above_context = row > 0 ? tile_context.frame_block_contexts().at(row - 1, column) : FrameBlockContext();
    auto left_context = column > tile_context.columns_start ? tile_context.frame_block_contexts().at(row, column - 1) : FrameBlockContext();
    auto block_context = BlockContext::create(tile_context, row, column, subsize);

    mode_info(block_context, above_context, left_context);
    auto had_residual_tokens = TRY(residual(block_context, above_context.is_available, left_context.is_available));
    if (block_context.is_inter_predicted() && subsize >= Block_8x8 && !had_residual_tokens)
        block_context.should_skip_residuals = true;

    for (size_t y = 0; y < block_context.contexts_view.height(); y++) {
        for (size_t x = 0; x < block_context.contexts_view.width(); x++) {
            auto sub_block_context = FrameBlockContext { true, block_context.should_skip_residuals, block_context.transform_size, block_context.y_prediction_mode(), block_context.sub_block_prediction_modes, block_context.interpolation_filter, block_context.reference_frame_types, block_context.sub_block_motion_vectors, block_context.segment_id };
            block_context.contexts_view.at(y, x) = sub_block_context;
            VERIFY(block_context.frame_block_contexts().at(row + y, column + x).transform_size == sub_block_context.transform_size);
        }
    }
    return {};
}

void Parser::mode_info(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context)
{
    if (block_context.frame_context.is_inter_predicted())
        inter_frame_mode_info(block_context, above_context, left_context);
    else
        intra_frame_mode_info(block_context, above_context, left_context);
}

void Parser::intra_frame_mode_info(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context)
{
    block_context.reference_frame_types = { ReferenceFrameType::None, ReferenceFrameType::None };
    VERIFY(!block_context.is_inter_predicted());
    set_intra_segment_id(block_context);
    block_context.should_skip_residuals = read_should_skip_residuals(block_context, above_context, left_context);
    block_context.transform_size = read_tx_size(block_context, above_context, left_context, true);
    // FIXME: This if statement is also present in parse_default_intra_mode. The selection of parameters for
    //        the probability table lookup should be inlined here.
    if (block_context.size >= Block_8x8) {
        auto mode = TreeParser::parse_default_intra_mode(block_context.decoder, *m_probability_tables, block_context.size, above_context, left_context, block_context.sub_block_prediction_modes, 0, 0);
        for (auto& block_sub_mode : block_context.sub_block_prediction_modes)
            block_sub_mode = mode;
    } else {
        auto size_in_sub_blocks = block_context.get_size_in_sub_blocks();
        for (auto idy = 0; idy < 2; idy += size_in_sub_blocks.height()) {
            for (auto idx = 0; idx < 2; idx += size_in_sub_blocks.width()) {
                auto sub_mode = TreeParser::parse_default_intra_mode(block_context.decoder, *m_probability_tables, block_context.size, above_context, left_context, block_context.sub_block_prediction_modes, idx, idy);

                for (auto y = 0; y < size_in_sub_blocks.height(); y++) {
                    for (auto x = 0; x < size_in_sub_blocks.width(); x++) {
                        auto index = (idy + y) * 2 + idx + x;
                        block_context.sub_block_prediction_modes[index] = sub_mode;
                    }
                }
            }
        }
    }
    block_context.uv_prediction_mode = TreeParser::parse_default_uv_mode(block_context.decoder, *m_probability_tables, block_context.y_prediction_mode());
}

void Parser::set_intra_segment_id(BlockContext& block_context)
{
    if (block_context.frame_context.segmentation_enabled && block_context.frame_context.use_full_segment_id_tree)
        block_context.segment_id = TreeParser::parse_segment_id(block_context.decoder, block_context.frame_context.full_segment_id_tree_probabilities);
    else
        block_context.segment_id = 0;
}

bool Parser::read_should_skip_residuals(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context)
{
    if (block_context.get_segment_feature(SegmentFeature::SkipResidualsOverride).enabled)
        return true;
    return TreeParser::parse_skip(block_context.decoder, *m_probability_tables, block_context.counter, above_context, left_context);
}

TransformSize Parser::read_tx_size(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context, bool allow_select)
{
    auto max_tx_size = max_txsize_lookup[block_context.size];
    if (allow_select && block_context.frame_context.transform_mode == TransformMode::Select && block_context.size >= Block_8x8)
        return (TreeParser::parse_tx_size(block_context.decoder, *m_probability_tables, block_context.counter, max_tx_size, above_context, left_context));
    return min(max_tx_size, tx_mode_to_biggest_tx_size[to_underlying(block_context.frame_context.transform_mode)]);
}

void Parser::inter_frame_mode_info(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context)
{
    set_inter_segment_id(block_context);
    block_context.should_skip_residuals = read_should_skip_residuals(block_context, above_context, left_context);
    auto is_inter = read_is_inter(block_context, above_context, left_context);
    block_context.transform_size = read_tx_size(block_context, above_context, left_context, !block_context.should_skip_residuals || !is_inter);
    if (is_inter) {
        inter_block_mode_info(block_context, above_context, left_context);
    } else {
        intra_block_mode_info(block_context);
    }
}

void Parser::set_inter_segment_id(BlockContext& block_context)
{
    if (!block_context.frame_context.segmentation_enabled) {
        block_context.segment_id = 0;
        return;
    }
    auto predicted_segment_id = get_segment_id(block_context);
    if (!block_context.frame_context.use_full_segment_id_tree) {
        block_context.segment_id = predicted_segment_id;
        return;
    }
    if (!block_context.frame_context.use_predicted_segment_id_tree) {
        block_context.segment_id = TreeParser::parse_segment_id(block_context.decoder, block_context.frame_context.full_segment_id_tree_probabilities);
        return;
    }

    auto above_segmentation_id = block_context.tile_context.above_segmentation_ids[block_context.row - block_context.tile_context.rows_start];
    auto left_segmentation_id = block_context.tile_context.left_segmentation_ids[block_context.column - block_context.tile_context.columns_start];
    auto seg_id_predicted = TreeParser::parse_segment_id_predicted(block_context.decoder, block_context.frame_context.predicted_segment_id_tree_probabilities, above_segmentation_id, left_segmentation_id);
    if (seg_id_predicted)
        block_context.segment_id = predicted_segment_id;
    else
        block_context.segment_id = TreeParser::parse_segment_id(block_context.decoder, block_context.frame_context.full_segment_id_tree_probabilities);

    // (7.4.1) AboveSegPredContext[ i ] only needs to be set to 0 for i = 0..MiCols-1.
    // This is taken care of by the slicing in BlockContext.
    block_context.above_segmentation_ids.fill(seg_id_predicted);
    // (7.4.1) LeftSegPredContext[ i ] only needs to be set to 0 for i = 0..MiRows-1.
    // This is taken care of by the slicing in BlockContext.
    block_context.left_segmentation_ids.fill(seg_id_predicted);
}

u8 Parser::get_segment_id(BlockContext const& block_context)
{
    auto bw = num_8x8_blocks_wide_lookup[block_context.size];
    auto bh = num_8x8_blocks_high_lookup[block_context.size];
    auto xmis = min(block_context.frame_context.columns() - block_context.column, (u32)bw);
    auto ymis = min(block_context.frame_context.rows() - block_context.row, (u32)bh);
    u8 segment = 7;
    for (size_t y = 0; y < ymis; y++) {
        for (size_t x = 0; x < xmis; x++) {
            segment = min(segment, m_previous_block_contexts.index_at(block_context.row + y, block_context.column + x));
        }
    }
    return segment;
}

bool Parser::read_is_inter(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context)
{
    auto reference_frame_override_feature = block_context.get_segment_feature(SegmentFeature::ReferenceFrameOverride);
    if (reference_frame_override_feature.enabled)
        return reference_frame_override_feature.value != ReferenceFrameType::None;
    return TreeParser::parse_block_is_inter_predicted(block_context.decoder, *m_probability_tables, block_context.counter, above_context, left_context);
}

void Parser::intra_block_mode_info(BlockContext& block_context)
{
    block_context.reference_frame_types = { ReferenceFrameType::None, ReferenceFrameType::None };
    VERIFY(!block_context.is_inter_predicted());
    auto& sub_modes = block_context.sub_block_prediction_modes;
    if (block_context.size >= Block_8x8) {
        auto mode = TreeParser::parse_intra_mode(block_context.decoder, *m_probability_tables, block_context.counter, block_context.size);
        for (auto& block_sub_mode : sub_modes)
            block_sub_mode = mode;
    } else {
        auto size_in_sub_blocks = block_context.get_size_in_sub_blocks();
        for (auto idy = 0; idy < 2; idy += size_in_sub_blocks.height()) {
            for (auto idx = 0; idx < 2; idx += size_in_sub_blocks.width()) {
                auto sub_intra_mode = TreeParser::parse_sub_intra_mode(block_context.decoder, *m_probability_tables, block_context.counter);
                for (auto y = 0; y < size_in_sub_blocks.height(); y++) {
                    for (auto x = 0; x < size_in_sub_blocks.width(); x++)
                        sub_modes[(idy + y) * 2 + idx + x] = sub_intra_mode;
                }
            }
        }
    }
    block_context.uv_prediction_mode = TreeParser::parse_uv_mode(block_context.decoder, *m_probability_tables, block_context.counter, block_context.y_prediction_mode());
}

static void select_best_reference_motion_vectors(BlockContext& block_context, MotionVectorPair reference_motion_vectors, BlockMotionVectorCandidates& candidates, ReferenceIndex);

void Parser::inter_block_mode_info(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context)
{
    read_ref_frames(block_context, above_context, left_context);
    VERIFY(block_context.is_inter_predicted());

    BlockMotionVectorCandidates motion_vector_candidates;
    auto reference_motion_vectors = find_reference_motion_vectors(block_context, block_context.reference_frame_types.primary, -1);
    select_best_reference_motion_vectors(block_context, reference_motion_vectors, motion_vector_candidates, ReferenceIndex::Primary);
    if (block_context.is_compound()) {
        auto reference_motion_vectors = find_reference_motion_vectors(block_context, block_context.reference_frame_types.secondary, -1);
        select_best_reference_motion_vectors(block_context, reference_motion_vectors, motion_vector_candidates, ReferenceIndex::Secondary);
    }

    if (block_context.get_segment_feature(SegmentFeature::SkipResidualsOverride).enabled) {
        block_context.y_prediction_mode() = PredictionMode::ZeroMv;
    } else if (block_context.size >= Block_8x8) {
        block_context.y_prediction_mode() = TreeParser::parse_inter_mode(block_context.decoder, *m_probability_tables, block_context.counter, block_context.mode_context[block_context.reference_frame_types.primary]);
    }
    if (block_context.frame_context.interpolation_filter == Switchable)
        block_context.interpolation_filter = TreeParser::parse_interpolation_filter(block_context.decoder, *m_probability_tables, block_context.counter, above_context, left_context);
    else
        block_context.interpolation_filter = block_context.frame_context.interpolation_filter;
    if (block_context.size < Block_8x8) {
        auto size_in_sub_blocks = block_context.get_size_in_sub_blocks();
        for (auto idy = 0; idy < 2; idy += size_in_sub_blocks.height()) {
            for (auto idx = 0; idx < 2; idx += size_in_sub_blocks.width()) {
                block_context.y_prediction_mode() = TreeParser::parse_inter_mode(block_context.decoder, *m_probability_tables, block_context.counter, block_context.mode_context[block_context.reference_frame_types.primary]);
                if (block_context.y_prediction_mode() == PredictionMode::NearestMv || block_context.y_prediction_mode() == PredictionMode::NearMv) {
                    select_best_sub_block_reference_motion_vectors(block_context, motion_vector_candidates, idy * 2 + idx, ReferenceIndex::Primary);
                    if (block_context.is_compound())
                        select_best_sub_block_reference_motion_vectors(block_context, motion_vector_candidates, idy * 2 + idx, ReferenceIndex::Secondary);
                }
                auto new_motion_vector_pair = get_motion_vector(block_context, motion_vector_candidates);
                for (auto y = 0; y < size_in_sub_blocks.height(); y++) {
                    for (auto x = 0; x < size_in_sub_blocks.width(); x++) {
                        auto sub_block_index = (idy + y) * 2 + idx + x;
                        block_context.sub_block_motion_vectors[sub_block_index] = new_motion_vector_pair;
                    }
                }
            }
        }
        return;
    }
    auto new_motion_vector_pair = get_motion_vector(block_context, motion_vector_candidates);
    for (auto block = 0; block < 4; block++)
        block_context.sub_block_motion_vectors[block] = new_motion_vector_pair;
}

void Parser::read_ref_frames(BlockContext& block_context, FrameBlockContext above_context, FrameBlockContext left_context)
{
    auto reference_frame_override_feature = block_context.get_segment_feature(SegmentFeature::ReferenceFrameOverride);
    if (reference_frame_override_feature.enabled) {
        block_context.reference_frame_types = { static_cast<ReferenceFrameType>(reference_frame_override_feature.value), ReferenceFrameType::None };
        return;
    }

    ReferenceMode compound_mode = block_context.frame_context.reference_mode;
    auto fixed_reference = block_context.frame_context.fixed_reference_type;
    if (compound_mode == ReferenceModeSelect)
        compound_mode = TreeParser::parse_comp_mode(block_context.decoder, *m_probability_tables, block_context.counter, fixed_reference, above_context, left_context);
    if (compound_mode == CompoundReference) {
        auto variable_references = block_context.frame_context.variable_reference_types;

        auto fixed_reference_index = ReferenceIndex::Primary;
        auto variable_reference_index = ReferenceIndex::Secondary;
        if (block_context.frame_context.reference_frame_sign_biases[fixed_reference])
            swap(fixed_reference_index, variable_reference_index);

        auto variable_reference_selection = TreeParser::parse_comp_ref(block_context.decoder, *m_probability_tables, block_context.counter, fixed_reference, variable_references, variable_reference_index, above_context, left_context);

        block_context.reference_frame_types[fixed_reference_index] = fixed_reference;
        block_context.reference_frame_types[variable_reference_index] = variable_references[variable_reference_selection];
        return;
    }

    // FIXME: Maybe consolidate this into a tree. Context is different between part 1 and 2 but still, it would look nice here.
    ReferenceFrameType primary_type = ReferenceFrameType::LastFrame;
    auto single_ref_p1 = TreeParser::parse_single_ref_part_1(block_context.decoder, *m_probability_tables, block_context.counter, above_context, left_context);
    if (single_ref_p1) {
        auto single_ref_p2 = TreeParser::parse_single_ref_part_2(block_context.decoder, *m_probability_tables, block_context.counter, above_context, left_context);
        primary_type = single_ref_p2 ? ReferenceFrameType::AltRefFrame : ReferenceFrameType::GoldenFrame;
    }
    block_context.reference_frame_types = { primary_type, ReferenceFrameType::None };
}

// assign_mv( isCompound ) in the spec.
MotionVectorPair Parser::get_motion_vector(BlockContext const& block_context, BlockMotionVectorCandidates const& candidates)
{
    MotionVectorPair result;
    auto read_one = [&](ReferenceIndex index) -> void {
        switch (block_context.y_prediction_mode()) {
        case PredictionMode::NewMv:
            result[index] = read_motion_vector(block_context, candidates, index);
            break;
        case PredictionMode::NearestMv:
            result[index] = candidates[index].nearest_vector;
            break;
        case PredictionMode::NearMv:
            result[index] = candidates[index].near_vector;
            break;
        default:
            result[index] = {};
            break;
        }
        return;
    };
    read_one(ReferenceIndex::Primary);
    if (block_context.is_compound())
        read_one(ReferenceIndex::Secondary);
    return result;
}

// use_mv_hp( deltaMv ) in the spec.
static bool should_use_high_precision_motion_vector(MotionVector const& delta_vector)
{
    return (abs(delta_vector.row()) >> 3) < COMPANDED_MVREF_THRESH && (abs(delta_vector.column()) >> 3) < COMPANDED_MVREF_THRESH;
}

// read_mv( ref ) in the spec.
MotionVector Parser::read_motion_vector(BlockContext const& block_context, BlockMotionVectorCandidates const& candidates, ReferenceIndex reference_index)
{
    auto use_high_precision = block_context.frame_context.high_precision_motion_vectors_allowed && should_use_high_precision_motion_vector(candidates[reference_index].best_vector);
    MotionVector delta_vector;
    auto joint = TreeParser::parse_motion_vector_joint(block_context.decoder, *m_probability_tables, block_context.counter);
    if ((joint & MotionVectorNonZeroRow) != 0)
        delta_vector.set_row(read_single_motion_vector_component(block_context.decoder, block_context.counter, 0, use_high_precision));
    if ((joint & MotionVectorNonZeroColumn) != 0)
        delta_vector.set_column(read_single_motion_vector_component(block_context.decoder, block_context.counter, 1, use_high_precision));

    return candidates[reference_index].best_vector + delta_vector;
}

// read_mv_component( comp ) in the spec.
i32 Parser::read_single_motion_vector_component(BooleanDecoder& decoder, SyntaxElementCounter& counter, u8 component, bool use_high_precision)
{
    auto mv_sign = TreeParser::parse_motion_vector_sign(decoder, *m_probability_tables, counter, component);
    auto mv_class = TreeParser::parse_motion_vector_class(decoder, *m_probability_tables, counter, component);
    u32 magnitude;
    if (mv_class == MvClass0) {
        auto mv_class0_bit = TreeParser::parse_motion_vector_class0_bit(decoder, *m_probability_tables, counter, component);
        auto mv_class0_fr = TreeParser::parse_motion_vector_class0_fr(decoder, *m_probability_tables, counter, component, mv_class0_bit);
        auto mv_class0_hp = TreeParser::parse_motion_vector_class0_hp(decoder, *m_probability_tables, counter, component, use_high_precision);
        magnitude = ((mv_class0_bit << 3) | (mv_class0_fr << 1) | mv_class0_hp) + 1;
    } else {
        u32 bits = 0;
        for (u8 i = 0; i < mv_class; i++) {
            auto mv_bit = TreeParser::parse_motion_vector_bit(decoder, *m_probability_tables, counter, component, i);
            bits |= mv_bit << i;
        }
        magnitude = CLASS0_SIZE << (mv_class + 2);
        auto mv_fr = TreeParser::parse_motion_vector_fr(decoder, *m_probability_tables, counter, component);
        auto mv_hp = TreeParser::parse_motion_vector_hp(decoder, *m_probability_tables, counter, component, use_high_precision);
        magnitude += ((bits << 3) | (mv_fr << 1) | mv_hp) + 1;
    }
    return (mv_sign ? -1 : 1) * static_cast<i32>(magnitude);
}

static TransformSize get_uv_transform_size(TransformSize transform_size, BlockSubsize size_for_plane)
{
    return min(transform_size, max_txsize_lookup[size_for_plane]);
}

static TransformSet select_transform_type(BlockContext const& block_context, u8 plane, TransformSize transform_size, u32 block_index)
{
    if (plane > 0 || transform_size == Transform_32x32)
        return TransformSet { TransformType::DCT, TransformType::DCT };
    if (transform_size == Transform_4x4) {
        if (block_context.frame_context.lossless || block_context.is_inter_predicted())
            return TransformSet { TransformType::DCT, TransformType::DCT };

        return mode_to_txfm_map[to_underlying(block_context.size < Block_8x8 ? block_context.sub_block_prediction_modes[block_index] : block_context.y_prediction_mode())];
    }

    return mode_to_txfm_map[to_underlying(block_context.y_prediction_mode())];
}

DecoderErrorOr<bool> Parser::residual(BlockContext& block_context, bool has_block_above, bool has_block_left)
{
    bool block_had_non_zero_tokens = false;
    Array<u8, 1024> token_cache;
    for (u8 plane = 0; plane < 3; plane++) {
        auto plane_subsampling_x = (plane > 0) ? block_context.frame_context.color_config.subsampling_x : false;
        auto plane_subsampling_y = (plane > 0) ? block_context.frame_context.color_config.subsampling_y : false;
        auto plane_size = get_subsampled_block_size(block_context.size, plane_subsampling_x, plane_subsampling_y);
        if (plane_size == Block_Invalid) {
            return DecoderError::corrupted("Invalid block size"sv);
        }
        auto transform_size = get_uv_transform_size(block_context.transform_size, plane_size);
        auto transform_size_in_sub_blocks = transform_size_to_sub_blocks(transform_size);
        auto block_size_in_sub_blocks = block_size_to_sub_blocks(plane_size);

        auto base_x_in_pixels = (blocks_to_pixels(block_context.column)) >> plane_subsampling_x;
        auto base_y_in_pixels = (blocks_to_pixels(block_context.row)) >> plane_subsampling_y;
        if (block_context.is_inter_predicted()) {
            if (block_context.size < Block_8x8) {
                for (auto y = 0; y < block_size_in_sub_blocks.height(); y++) {
                    for (auto x = 0; x < block_size_in_sub_blocks.width(); x++) {
                        TRY(m_decoder.predict_inter(plane, block_context, base_x_in_pixels + sub_blocks_to_pixels(x), base_y_in_pixels + sub_blocks_to_pixels(y), sub_blocks_to_pixels(1), sub_blocks_to_pixels(1), (y * block_size_in_sub_blocks.width()) + x));
                    }
                }
            } else {
                TRY(m_decoder.predict_inter(plane, block_context, base_x_in_pixels, base_y_in_pixels, sub_blocks_to_pixels(block_size_in_sub_blocks.width()), sub_blocks_to_pixels(block_size_in_sub_blocks.height()), 0));
            }
        }

        auto frame_right_in_pixels = (blocks_to_pixels(block_context.frame_context.columns())) >> plane_subsampling_x;
        auto frame_bottom_in_pixels = (blocks_to_pixels(block_context.frame_context.rows())) >> plane_subsampling_y;

        auto sub_block_index = 0;
        for (u32 y = 0; y < block_size_in_sub_blocks.height(); y += transform_size_in_sub_blocks) {
            for (u32 x = 0; x < block_size_in_sub_blocks.width(); x += transform_size_in_sub_blocks) {
                auto transform_x_in_px = base_x_in_pixels + sub_blocks_to_pixels(x);
                auto transform_y_in_px = base_y_in_pixels + sub_blocks_to_pixels(y);

                auto sub_block_had_non_zero_tokens = false;
                if (transform_x_in_px < frame_right_in_pixels && transform_y_in_px < frame_bottom_in_pixels) {
                    if (!block_context.is_inter_predicted())
                        TRY(m_decoder.predict_intra(plane, block_context, transform_x_in_px, transform_y_in_px, has_block_left || x > 0, has_block_above || y > 0, (x + transform_size_in_sub_blocks) < block_size_in_sub_blocks.width(), transform_size, sub_block_index));
                    if (!block_context.should_skip_residuals) {
                        auto transform_set = select_transform_type(block_context, plane, transform_size, sub_block_index);
                        sub_block_had_non_zero_tokens = tokens(block_context, plane, x, y, transform_size, transform_set, token_cache);
                        block_had_non_zero_tokens = block_had_non_zero_tokens || sub_block_had_non_zero_tokens;
                        TRY(m_decoder.reconstruct(plane, block_context, transform_x_in_px, transform_y_in_px, transform_size, transform_set));
                    }
                }

                auto& above_sub_block_tokens = block_context.above_non_zero_tokens[plane];
                auto transform_right_in_sub_blocks = min(x + transform_size_in_sub_blocks, above_sub_block_tokens.size());
                for (size_t inside_x = x; inside_x < transform_right_in_sub_blocks; inside_x++)
                    above_sub_block_tokens[inside_x] = sub_block_had_non_zero_tokens;

                auto& left_sub_block_context = block_context.left_non_zero_tokens[plane];
                auto transform_bottom_in_sub_blocks = min(y + transform_size_in_sub_blocks, left_sub_block_context.size());
                for (size_t inside_y = y; inside_y < transform_bottom_in_sub_blocks; inside_y++)
                    left_sub_block_context[inside_y] = sub_block_had_non_zero_tokens;

                sub_block_index++;
            }
        }
    }
    return block_had_non_zero_tokens;
}

static u16 const* get_scan(TransformSize transform_size, TransformSet transform_set)
{
    constexpr TransformSet adst_dct { TransformType::ADST, TransformType::DCT };
    constexpr TransformSet dct_adst { TransformType::DCT, TransformType::ADST };

    if (transform_size == Transform_4x4) {
        if (transform_set == adst_dct)
            return row_scan_4x4;
        if (transform_set == dct_adst)
            return col_scan_4x4;
        return default_scan_4x4;
    }
    if (transform_size == Transform_8x8) {
        if (transform_set == adst_dct)
            return row_scan_8x8;
        if (transform_set == dct_adst)
            return col_scan_8x8;
        return default_scan_8x8;
    }
    if (transform_size == Transform_16x16) {
        if (transform_set == adst_dct)
            return row_scan_16x16;
        if (transform_set == dct_adst)
            return col_scan_16x16;
        return default_scan_16x16;
    }
    return default_scan_32x32;
}

bool Parser::tokens(BlockContext& block_context, size_t plane, u32 sub_block_column, u32 sub_block_row, TransformSize transform_size, TransformSet transform_set, Array<u8, 1024> token_cache)
{
    block_context.residual_tokens.fill(0);

    auto const* scan = get_scan(transform_size, transform_set);

    auto check_for_more_coefficients = true;
    u16 coef_index = 0;
    u16 transform_pixel_count = 16 << (transform_size << 1);
    for (; coef_index < transform_pixel_count; coef_index++) {
        auto band = (transform_size == Transform_4x4) ? coefband_4x4[coef_index] : coefband_8x8plus[coef_index];
        auto token_position = scan[coef_index];
        TokensContext tokens_context;
        if (coef_index == 0)
            tokens_context = TreeParser::get_context_for_first_token(block_context.above_non_zero_tokens, block_context.left_non_zero_tokens, transform_size, plane, sub_block_column, sub_block_row, block_context.is_inter_predicted(), band);
        else
            tokens_context = TreeParser::get_context_for_other_tokens(token_cache, transform_size, transform_set, plane, token_position, block_context.is_inter_predicted(), band);

        if (check_for_more_coefficients && !TreeParser::parse_more_coefficients(block_context.decoder, *m_probability_tables, block_context.counter, tokens_context))
            break;

        auto token = TreeParser::parse_token(block_context.decoder, *m_probability_tables, block_context.counter, tokens_context);
        token_cache[token_position] = energy_class[token];

        i32 coef;
        if (token == ZeroToken) {
            coef = 0;
            check_for_more_coefficients = false;
        } else {
            coef = read_coef(block_context.decoder, block_context.frame_context.color_config.bit_depth, token);
            check_for_more_coefficients = true;
        }
        block_context.residual_tokens[token_position] = coef;
    }

    return coef_index > 0;
}

i32 Parser::read_coef(BooleanDecoder& decoder, u8 bit_depth, Token token)
{
    auto cat = extra_bits[token][0];
    auto num_extra = extra_bits[token][1];
    i32 coef = extra_bits[token][2];
    if (token == DctValCat6) {
        for (size_t e = 0; e < (u8)(bit_depth - 8); e++) {
            auto high_bit = decoder.read_bool(255);
            coef += high_bit << (5 + bit_depth - e);
        }
    }
    for (size_t e = 0; e < num_extra; e++) {
        auto coef_bit = decoder.read_bool(cat_probs[cat][e]);
        coef += coef_bit << (num_extra - 1 - e);
    }
    bool sign_bit = decoder.read_literal(1);
    coef = sign_bit ? -coef : coef;
    return coef;
}

// is_inside( candidateR, candidateC ) in the spec.
static bool motion_vector_is_inside_tile(TileContext const& tile_context, MotionVector vector)
{
    if (vector.row() < 0)
        return false;
    if (vector.column() < 0)
        return false;
    u32 row_positive = vector.row();
    u32 column_positive = vector.column();
    return row_positive < tile_context.frame_context.rows() && column_positive >= tile_context.columns_start && column_positive < tile_context.columns_end;
}

// add_mv_ref_list( refList ) in the spec.
static void add_motion_vector_to_list_deduped(MotionVector const& vector, Vector<MotionVector, 2>& list)
{
    if (list.size() >= 2)
        return;
    if (list.size() == 1 && vector == list[0])
        return;

    list.append(vector);
}

// get_block_mv( candidateR, candidateC, refList, usePrev ) in the spec.
MotionVectorCandidate Parser::get_motion_vector_from_current_or_previous_frame(BlockContext const& block_context, MotionVector candidate_vector, ReferenceIndex reference_index, bool use_prev)
{
    if (use_prev) {
        auto const& prev_context = m_previous_block_contexts.at(candidate_vector.row(), candidate_vector.column());
        return { prev_context.ref_frames[reference_index], prev_context.primary_motion_vector_pair[reference_index] };
    }

    auto const& current_context = block_context.frame_block_contexts().at(candidate_vector.row(), candidate_vector.column());
    return { current_context.ref_frames[reference_index], current_context.primary_motion_vector_pair()[reference_index] };
}

// if_same_ref_frame_add_mv( candidateR, candidateC, refFrame, usePrev ) in the spec.
void Parser::add_motion_vector_if_reference_frame_type_is_same(BlockContext const& block_context, MotionVector candidate_vector, ReferenceFrameType ref_frame, Vector<MotionVector, 2>& list, bool use_prev)
{
    for (auto i = 0u; i < 2; i++) {
        auto candidate = get_motion_vector_from_current_or_previous_frame(block_context, candidate_vector, static_cast<ReferenceIndex>(i), use_prev);
        if (candidate.type == ref_frame) {
            add_motion_vector_to_list_deduped(candidate.vector, list);
            return;
        }
    }
}

// scale_mv( refList, refFrame ) in the spec.
static void apply_sign_bias_to_motion_vector(FrameContext const& frame_context, MotionVectorCandidate& candidate, ReferenceFrameType ref_frame)
{
    if (frame_context.reference_frame_sign_biases[candidate.type] != frame_context.reference_frame_sign_biases[ref_frame])
        candidate.vector *= -1;
}

// if_diff_ref_frame_add_mv( candidateR, candidateC, refFrame, usePrev ) in the spec.
void Parser::add_motion_vector_if_reference_frame_type_is_different(BlockContext const& block_context, MotionVector candidate_vector, ReferenceFrameType ref_frame, Vector<MotionVector, 2>& list, bool use_prev)
{
    auto first_candidate = get_motion_vector_from_current_or_previous_frame(block_context, candidate_vector, ReferenceIndex::Primary, use_prev);
    if (first_candidate.type > ReferenceFrameType::None && first_candidate.type != ref_frame) {
        apply_sign_bias_to_motion_vector(block_context.frame_context, first_candidate, ref_frame);
        add_motion_vector_to_list_deduped(first_candidate.vector, list);
    }

    auto second_candidate = get_motion_vector_from_current_or_previous_frame(block_context, candidate_vector, ReferenceIndex::Secondary, use_prev);
    auto mvs_are_same = first_candidate.vector == second_candidate.vector;
    if (second_candidate.type > ReferenceFrameType::None && second_candidate.type != ref_frame && !mvs_are_same) {
        apply_sign_bias_to_motion_vector(block_context.frame_context, second_candidate, ref_frame);
        add_motion_vector_to_list_deduped(second_candidate.vector, list);
    }
}

// This function handles both clamp_mv_row( mvec, border ) and clamp_mv_col( mvec, border ) in the spec.
static MotionVector clamp_motion_vector(BlockContext const& block_context, MotionVector vector, i32 border)
{
    i32 blocks_high = num_8x8_blocks_high_lookup[block_context.size];
    // Casts must be done here to prevent subtraction underflow from wrapping the values.
    i32 mb_to_top_edge = -8 * (static_cast<i32>(block_context.row) * MI_SIZE);
    i32 mb_to_bottom_edge = 8 * ((static_cast<i32>(block_context.frame_context.rows()) - blocks_high - static_cast<i32>(block_context.row)) * MI_SIZE);

    i32 blocks_wide = num_8x8_blocks_wide_lookup[block_context.size];
    i32 mb_to_left_edge = -8 * (static_cast<i32>(block_context.column) * MI_SIZE);
    i32 mb_to_right_edge = 8 * ((static_cast<i32>(block_context.frame_context.columns()) - blocks_wide - static_cast<i32>(block_context.column)) * MI_SIZE);

    return {
        clip_3(mb_to_top_edge - border, mb_to_bottom_edge + border, vector.row()),
        clip_3(mb_to_left_edge - border, mb_to_right_edge + border, vector.column())
    };
}

// 6.5.1 Find MV refs syntax
// find_mv_refs( refFrame, block ) in the spec.
MotionVectorPair Parser::find_reference_motion_vectors(BlockContext& block_context, ReferenceFrameType reference_frame, i32 block)
{
    // FIXME: We should be able to change behavior based on the reference motion vector that will be selected.
    //        If block_context.y_prediction_mode() != NearMv, then we only need the first motion vector that is added to our result.
    //        This behavior should combine this function with select_best_reference_motion_vectors(). When that is done, check whether
    //        the motion vector clamping in that function is always a larger area than in this function. If so, we can drop that call.
    bool different_ref_found = false;
    u8 context_counter = 0;

    Vector<MotionVector, 2> list;

    MotionVector base_coordinates = MotionVector(block_context.row, block_context.column);

    for (auto i = 0u; i < 2; i++) {
        auto offset_vector = mv_ref_blocks[block_context.size][i];
        auto candidate = base_coordinates + offset_vector;

        if (motion_vector_is_inside_tile(block_context.tile_context, candidate)) {
            different_ref_found = true;
            auto context = block_context.frame_block_contexts().at(candidate.row(), candidate.column());
            context_counter += mode_2_counter[to_underlying(context.y_mode)];

            for (auto i = 0u; i < 2; i++) {
                auto reference_index = static_cast<ReferenceIndex>(i);
                if (context.ref_frames[reference_index] == reference_frame) {
                    // This section up until add_mv_ref_list() is defined in spec as get_sub_block_mv().
                    constexpr u8 idx_n_column_to_subblock[4][2] = {
                        { 1, 2 },
                        { 1, 3 },
                        { 3, 2 },
                        { 3, 3 }
                    };
                    auto index = block >= 0 ? idx_n_column_to_subblock[block][offset_vector.column() == 0] : 3;

                    add_motion_vector_to_list_deduped(context.sub_block_motion_vectors[index][reference_index], list);
                    break;
                }
            }
        }
    }
    block_context.mode_context[reference_frame] = counter_to_context[context_counter];

    for (auto i = 2u; i < MVREF_NEIGHBORS; i++) {
        MotionVector candidate = base_coordinates + mv_ref_blocks[block_context.size][i];
        if (motion_vector_is_inside_tile(block_context.tile_context, candidate)) {
            different_ref_found = true;
            add_motion_vector_if_reference_frame_type_is_same(block_context, candidate, reference_frame, list, false);
        }
    }
    if (block_context.frame_context.use_previous_frame_motion_vectors)
        add_motion_vector_if_reference_frame_type_is_same(block_context, base_coordinates, reference_frame, list, true);

    if (different_ref_found) {
        for (auto i = 0u; i < MVREF_NEIGHBORS; i++) {
            MotionVector candidate = base_coordinates + mv_ref_blocks[block_context.size][i];
            if (motion_vector_is_inside_tile(block_context.tile_context, candidate))
                add_motion_vector_if_reference_frame_type_is_different(block_context, candidate, reference_frame, list, false);
        }
    }
    if (block_context.frame_context.use_previous_frame_motion_vectors)
        add_motion_vector_if_reference_frame_type_is_different(block_context, base_coordinates, reference_frame, list, true);

    for (auto i = 0u; i < list.size(); i++) {
        // clamp_mv_ref( i ) in the spec.
        list[i] = clamp_motion_vector(block_context, list[i], MV_BORDER);
    }

    MotionVectorPair result;
    for (auto i = 0u; i < list.size(); i++)
        result[static_cast<ReferenceIndex>(i)] = list[i];
    result.primary = clamp_motion_vector(block_context, result.primary, MV_BORDER);
    result.secondary = clamp_motion_vector(block_context, result.secondary, MV_BORDER);
    return result;
}

// find_best_ref_mvs( refList ) in the spec.
static void select_best_reference_motion_vectors(BlockContext& block_context, MotionVectorPair reference_motion_vectors, BlockMotionVectorCandidates& candidates, ReferenceIndex reference_index)
{
    auto adjust_and_clamp_vector = [&](MotionVector& vector) {
        auto delta_row = vector.row();
        auto delta_column = vector.column();
        if (!block_context.frame_context.high_precision_motion_vectors_allowed || !should_use_high_precision_motion_vector(vector)) {
            if ((delta_row & 1) != 0)
                delta_row += delta_row > 0 ? -1 : 1;
            if ((delta_column & 1) != 0)
                delta_column += delta_column > 0 ? -1 : 1;
        }
        vector = { delta_row, delta_column };
        vector = clamp_motion_vector(block_context, vector, (BORDERINPIXELS - INTERP_EXTEND) << 3);
    };
    adjust_and_clamp_vector(reference_motion_vectors.primary);
    adjust_and_clamp_vector(reference_motion_vectors.secondary);

    candidates[reference_index].nearest_vector = reference_motion_vectors.primary;
    candidates[reference_index].near_vector = reference_motion_vectors.secondary;
    candidates[reference_index].best_vector = reference_motion_vectors.primary;
}

// append_sub8x8_mvs( block, refList ) in the spec.
void Parser::select_best_sub_block_reference_motion_vectors(BlockContext& block_context, BlockMotionVectorCandidates& candidates, i32 block, ReferenceIndex reference_index)
{
    Array<MotionVector, 2> sub_8x8_mvs;
    MotionVectorPair reference_motion_vectors = find_reference_motion_vectors(block_context, block_context.reference_frame_types[reference_index], block);
    auto destination_index = 0;
    if (block == 0) {
        sub_8x8_mvs[destination_index++] = reference_motion_vectors.primary;
        sub_8x8_mvs[destination_index++] = reference_motion_vectors.secondary;
    } else if (block <= 2) {
        sub_8x8_mvs[destination_index++] = block_context.sub_block_motion_vectors[0][reference_index];
    } else {
        sub_8x8_mvs[destination_index++] = block_context.sub_block_motion_vectors[2][reference_index];
        for (auto index = 1; index >= 0 && destination_index < 2; index--) {
            auto block_vector = block_context.sub_block_motion_vectors[index][reference_index];
            if (block_vector != sub_8x8_mvs[0])
                sub_8x8_mvs[destination_index++] = block_vector;
        }
    }

    for (auto n = 0u; n < 2 && destination_index < 2; n++) {
        auto ref_list_vector = reference_motion_vectors[static_cast<ReferenceIndex>(n)];
        if (ref_list_vector != sub_8x8_mvs[0])
            sub_8x8_mvs[destination_index++] = ref_list_vector;
    }

    if (destination_index < 2)
        sub_8x8_mvs[destination_index++] = {};
    candidates[reference_index].nearest_vector = sub_8x8_mvs[0];
    candidates[reference_index].near_vector = sub_8x8_mvs[1];
}

}
