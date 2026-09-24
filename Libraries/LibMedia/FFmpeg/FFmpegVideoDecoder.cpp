/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/VideoFrame.h>

#include "FFmpegFunctions.h"
#include "FFmpegHelpers.h"
#include "FFmpegVideoDecoder.h"

namespace Media::FFmpeg {

static constexpr size_t MAXIMUM_FRAMES_IN_FLIGHT = 256;

Optional<DecoderCapabilities> FFmpegVideoDecoder::capabilities(FFmpegFunctions const& functions, ParsedCodec const& codec)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Video)
        return {};
    if (!functions.avcodec_find_decoder(ffmpeg_codec_id_from_media_codec_id(codec.codec_id())))
        return {};
    return DecoderCapabilities { .smooth = true, .power_efficient = false };
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(FFmpegFunctions const& functions, CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            functions.avcodec_free_context(&codec_context);
            functions.av_packet_free(&packet);
            functions.av_frame_free(&frame);
        }
    };

    auto ff_codec_id = ffmpeg_codec_id_from_media_codec_id(codec_id);
    auto const* codec = functions.avcodec_find_decoder(ff_codec_id);
    if (!codec)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find FFmpeg decoder for codec {}", codec_id);

    codec_context = functions.avcodec_alloc_context3(codec);
    if (!codec_context)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    TRY(set_codec_initialization_data(functions, *codec_context, codec_id, codec_initialization_data));
    VERIFY(functions.av_opt_set_int(codec_context, "threads", min(Core::System::hardware_concurrency(), 4u), 0) == 0);

    if (functions.avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", codec_id);

    packet = functions.av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = functions.av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    auto frame_pool_result = VideoFramePool::create();
    if (frame_pool_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to create a video frame pool: {}", frame_pool_result.release_error());

    memory_guard.disarm();
    return DECODER_TRY_ALLOC(try_make<FFmpegVideoDecoder>(functions, codec_context, packet, frame, frame_pool_result.release_value()));
}

Optional<DecoderCapabilities> FFmpegVideoDecoder::capabilities(ParsedCodec const& codec)
{
    return capabilities(FFmpegFunctions::bundled(), codec);
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return try_create(FFmpegFunctions::bundled(), codec_id, codec_initialization_data);
}

FFmpegVideoDecoder::FFmpegVideoDecoder(FFmpegFunctions const& functions, AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, NonnullRefPtr<VideoFramePool> frame_pool)
    : m_functions(functions)
    , m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
    , m_frame_pool(move(frame_pool))
{
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    m_frame_pool->shed_storage();
    m_functions.av_packet_free(&m_packet);
    m_functions.av_frame_free(&m_frame);
    m_functions.avcodec_free_context(&m_codec_context);
}

DecoderErrorOr<void> FFmpegVideoDecoder::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    auto coded_data = coded_frame.data();
    VERIFY(coded_data.size() < NumericLimits<int>::max());

    auto key = m_next_frame_key++;
    m_packet->data = const_cast<u8*>(coded_data.data());
    m_packet->size = static_cast<int>(coded_data.size());
    m_packet->pts = static_cast<i64>(key);
    m_packet->dts = AV_NOPTS_VALUE;

    if (m_frames_in_flight.size() >= MAXIMUM_FRAMES_IN_FLIGHT) {
        auto stale_frame_count = m_frames_in_flight.size();
        m_frames_in_flight.remove_all_matching([&](u64 in_flight_key, auto const&) { return in_flight_key + MAXIMUM_FRAMES_IN_FLIGHT <= key; });
        stale_frame_count -= m_frames_in_flight.size();
        if (stale_frame_count > 0)
            dbgln("FFmpegVideoDecoder: {} frames were never output", stale_frame_count);
    }
    DECODER_TRY_ALLOC(m_frames_in_flight.try_set(key, { coded_frame.presentation_timestamp(), coded_frame.duration(), intent }));

    ScopeGuard clear_packet_side_data { [&] { m_functions.av_packet_free_side_data(m_packet); } };
    auto new_codec_configuration = coded_frame.new_codec_configuration();
    if (new_codec_configuration.has_value() && !new_codec_configuration->is_empty())
        TRY(add_new_extradata_to_packet(m_functions, *m_packet, *new_codec_configuration));

    auto result = m_functions.avcodec_send_packet(m_codec_context, m_packet);
    switch (result) {
    case 0:
        return {};
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "FFmpeg decoder cannot decode any more data until frames have been retrieved"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
    case AVERROR(ENOMEM):
        return DecoderError::with_description(DecoderErrorCategory::Memory, "FFmpeg codec ran out of internal memory"sv);
    default:
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "FFmpeg codec reports that the data is corrupted"sv);
    }
}

void FFmpegVideoDecoder::signal_end_of_stream()
{
    m_packet->data = nullptr;
    m_packet->size = 0;
    m_packet->pts = 0;
    m_packet->dts = 0;

    auto result = m_functions.avcodec_send_packet(m_codec_context, m_packet);
    VERIFY(result == 0 || result == AVERROR_EOF);
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> FFmpegVideoDecoder::take_next_output(CodingIndependentCodePoints const& container_cicp, [[maybe_unused]] Optional<AK::Duration> target)
{
    while (!m_pending_frame.has_value()) {
        auto result = m_functions.avcodec_receive_frame(m_codec_context, m_frame);

        switch (result) {
        case 0: {
            auto in_flight_frame = m_frames_in_flight.take(static_cast<u64>(m_frame->pts));
            if (!in_flight_frame.has_value()) {
                dbgln("FFmpegVideoDecoder: Dropping a frame whose timing was forgotten");
                continue;
            }
            if (in_flight_frame->intent == DecodeIntent::Reference)
                continue;
            m_pending_frame = in_flight_frame.release_value();
            break;
        }
        case AVERROR(EAGAIN):
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder has no frames available, send more input"sv);
        case AVERROR_EOF:
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
        case AVERROR(EINVAL):
            return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
        default:
            return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg codec encountered an unexpected error retrieving frames with code {:x}", result);
        }
    }

    auto color_primaries = static_cast<ColorPrimaries>(codec_context_option("color_primaries"));
    auto transfer_characteristics = static_cast<TransferCharacteristics>(codec_context_option("color_trc"));
    auto matrix_coefficients = static_cast<MatrixCoefficients>(codec_context_option("colorspace"));
    auto color_range = [&] {
        switch (codec_context_option("color_range")) {
        case AVColorRange::AVCOL_RANGE_MPEG:
            return VideoFullRangeFlag::Studio;
        case AVColorRange::AVCOL_RANGE_JPEG:
            return VideoFullRangeFlag::Full;
        default:
            return VideoFullRangeFlag::Unspecified;
        }
    }();
    auto cicp = container_cicp;
    cicp.adopt_specified_values({ color_primaries, transfer_characteristics, matrix_coefficients, color_range });

    auto pixel_format = static_cast<AVPixelFormat>(m_frame->format);
    auto bit_depth = [&]() -> Optional<u8> {
        switch (pixel_format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ444P:
            return 8;
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV444P10:
            return 10;
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUV444P12:
            return 12;
        default:
            return {};
        }
    }();
    if (!bit_depth.has_value())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg decoder produced an unsupported pixel format {}", to_underlying(pixel_format));

    auto subsampling = [&] {
        switch (pixel_format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUVJ420P:
            return Subsampling::yuv420();
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUVJ422P:
            return Subsampling::yuv422();
        default:
            return Subsampling::yuv444();
        }
    }();

    Gfx::IntSize size { m_frame->width, m_frame->height };
    auto layout_result = frame_plane_layout(size, *bit_depth, subsampling);
    if (layout_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Invalid, "Failed to compute video frame plane layout: {}", layout_result.release_error());
    auto layout = layout_result.release_value();

    auto acquired_slot = m_frame_pool->try_acquire(layout.total_byte_count);
    if (!acquired_slot.has_value())
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every video frame slot is held elsewhere"sv);

    auto pool_slot_result = m_frame_pool->try_adopt_acquired_slot(*acquired_slot);
    if (pool_slot_result.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a pooled frame slot reference"sv);
    auto pool_slot = pool_slot_result.release_value();

    auto yuv_data = DECODER_TRY_ALLOC(Gfx::YUVData::create(size, *bit_depth, subsampling, cicp,
        acquired_slot->bytes.slice(0, layout.y_size),
        acquired_slot->bytes.slice(layout.u_offset, layout.u_size),
        acquired_slot->bytes.slice(layout.v_offset, layout.v_size)));
    TRY(copy_pending_frame_into(yuv_data));

    auto frame = DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(m_pending_frame->timestamp, m_pending_frame->duration, size.to_type<u32>(), *bit_depth, subsampling, cicp, move(pool_slot)));
    m_pending_frame.clear();
    return frame;
}

DecoderErrorOr<void> FFmpegVideoDecoder::copy_pending_frame_into(Gfx::YUVData& yuv_data)
{
    auto size = Gfx::Size<u32> { m_frame->width, m_frame->height };
    auto y_plane_size = size.to_type<size_t>();
    auto uv_plane_size = yuv_data.subsampling().subsampled_size(size).to_type<size_t>();

    Bytes buffers[] = { yuv_data.y_data(), yuv_data.u_data(), yuv_data.v_data() };
    Gfx::Size<size_t> plane_sizes[] = { y_plane_size, uv_plane_size, uv_plane_size };

    auto component_size = yuv_data.bit_depth() <= 8 ? 1 : 2;

    for (u32 plane = 0; plane < 3; plane++) {
        VERIFY(m_frame->linesize[plane] != 0);
        if (m_frame->linesize[plane] < 0)
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Reversed scanlines are not supported"sv);

        auto plane_size = plane_sizes[plane];
        auto const* source = m_frame->data[plane];
        VERIFY(source != nullptr);
        auto destination = buffers[plane];

        auto output_line_size = plane_size.width() * component_size;
        VERIFY(output_line_size <= static_cast<size_t>(m_frame->linesize[plane]));
        VERIFY(destination.size() >= output_line_size * plane_size.height());

        auto* dest_ptr = destination.data();
        for (size_t row = 0; row < plane_size.height(); row++) {
            memcpy(dest_ptr, source, output_line_size);
            source += m_frame->linesize[plane];
            dest_ptr += output_line_size;
        }
    }

    return {};
}

i64 FFmpegVideoDecoder::codec_context_option(char const* name) const
{
    i64 value = 0;
    VERIFY(m_functions.av_opt_get_int(m_codec_context, name, 0, &value) == 0);
    return value;
}

void FFmpegVideoDecoder::flush()
{
    m_functions.avcodec_flush_buffers(m_codec_context);
    m_pending_frame.clear();
    m_frames_in_flight.clear();
}

}
