/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>

#include "FFmpegHelpers.h"
#include "FFmpegVideoDecoder.h"

namespace Media::FFmpeg {

static AVPixelFormat negotiate_output_format(AVCodecContext*, AVPixelFormat const* formats)
{
    while (*formats >= 0) {
        switch (*formats) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_YUV444P12:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ444P:
            return *formats;
        default:
            break;
        }
        formats++;
    }
    return AV_PIX_FMT_NONE;
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            avcodec_free_context(&codec_context);
            av_packet_free(&packet);
            av_frame_free(&frame);
        }
    };

    auto ff_codec_id = ffmpeg_codec_id_from_media_codec_id(codec_id);
    auto const* codec = avcodec_find_decoder(ff_codec_id);
    if (!codec)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find FFmpeg decoder for codec {}", codec_id);

    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    codec_context->get_format = negotiate_output_format;
    codec_context->time_base = { 1, 1'000'000 };
    codec_context->thread_count = static_cast<int>(min(Core::System::hardware_concurrency(), 4));

    if (!codec_initialization_data.is_empty()) {
        if (codec_initialization_data.size() > NumericLimits<int>::max())
            return DecoderError::corrupted("Codec initialization data is too large"sv);

        codec_context->extradata = static_cast<u8*>(av_malloc(codec_initialization_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codec_context->extradata)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate codec initialization data buffer for FFmpeg codec"sv);

        memcpy(codec_context->extradata, codec_initialization_data.data(), codec_initialization_data.size());
        codec_context->extradata_size = static_cast<int>(codec_initialization_data.size());
    }

    if (avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", codec_id);

    packet = av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    memory_guard.disarm();
    return DECODER_TRY_ALLOC(try_make<FFmpegVideoDecoder>(codec_context, packet, frame));
}

FFmpegVideoDecoder::FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame)
    : m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
{
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    av_packet_free(&m_packet);
    av_frame_free(&m_frame);
    avcodec_free_context(&m_codec_context);
}

DecoderErrorOr<void> FFmpegVideoDecoder::receive_coded_data(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data)
{
    VERIFY(coded_data.size() < NumericLimits<int>::max());

    m_packet->data = const_cast<u8*>(coded_data.data());
    m_packet->size = static_cast<int>(coded_data.size());
    m_packet->pts = timestamp.to_microseconds();
    m_packet->dts = m_packet->pts;
    m_packet->duration = duration.to_microseconds();

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    switch (result) {
    case 0:
        return {};
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder cannot decode any more data until frames have been retrieved"sv);
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

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    VERIFY(result == 0 || result == AVERROR_EOF);
}

DecoderErrorOr<NonnullOwnPtr<VideoFrame>> FFmpegVideoDecoder::get_decoded_frame(CodingIndependentCodePoints const& container_cicp)
{
    auto result = avcodec_receive_frame(m_codec_context, m_frame);

    switch (result) {
    case 0: {
        auto color_primaries = static_cast<ColorPrimaries>(m_frame->color_primaries);
        auto transfer_characteristics = static_cast<TransferCharacteristics>(m_frame->color_trc);
        auto matrix_coefficients = static_cast<MatrixCoefficients>(m_frame->colorspace);
        auto color_range = [&] {
            switch (m_frame->color_range) {
            case AVColorRange::AVCOL_RANGE_MPEG:
                return VideoFullRangeFlag::Studio;
            case AVColorRange::AVCOL_RANGE_JPEG:
                return VideoFullRangeFlag::Full;
            default:
                return VideoFullRangeFlag::Unspecified;
            }
        }();
        auto cicp = CodingIndependentCodePoints { color_primaries, transfer_characteristics, matrix_coefficients, color_range };

        cicp.adopt_specified_values(container_cicp);

        // BT.470 M, B/G, BT.601, BT.709 and BT.2020 have a similar transfer function to sRGB, so other applications
        // (Chromium, VLC) forgo transfer characteristics conversion. We will emulate that behavior by
        // handling those as sRGB instead, which causes no transfer function change in the output,
        // unless display color management is later implemented.
        switch (cicp.transfer_characteristics()) {
        case TransferCharacteristics::BT470BG:
        case TransferCharacteristics::BT470M:
        case TransferCharacteristics::BT601:
        case TransferCharacteristics::BT709:
        case TransferCharacteristics::BT2020BitDepth10:
        case TransferCharacteristics::BT2020BitDepth12:
            cicp.set_transfer_characteristics(TransferCharacteristics::SRGB);
            break;
        default:
            break;
        }

        size_t bit_depth = [&] {
            switch (m_frame->format) {
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
                VERIFY_NOT_REACHED();
            }
        }();

        auto subsampling = [&]() -> Subsampling {
            switch (m_frame->format) {
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUV420P10:
            case AV_PIX_FMT_YUV420P12:
            case AV_PIX_FMT_YUVJ420P:
                return { true, true };
            case AV_PIX_FMT_YUV422P:
            case AV_PIX_FMT_YUV422P10:
            case AV_PIX_FMT_YUV422P12:
            case AV_PIX_FMT_YUVJ422P:
                return { true, false };
            case AV_PIX_FMT_YUV444P:
            case AV_PIX_FMT_YUV444P10:
            case AV_PIX_FMT_YUV444P12:
            case AV_PIX_FMT_YUVJ444P:
                return { false, false };
            default:
                VERIFY_NOT_REACHED();
            }
        }();

        auto size = Gfx::Size<u32> { m_frame->width, m_frame->height };
        auto gfx_size = Gfx::IntSize { m_frame->width, m_frame->height };

        auto timestamp = AK::Duration::from_microseconds(m_frame->pts);
        auto duration = AK::Duration::from_microseconds(m_frame->duration);

        auto yuv_data = DECODER_TRY_ALLOC(Gfx::YUVData::create(gfx_size, bit_depth, subsampling, cicp));

        auto y_plane_size = size.to_type<size_t>();
        auto uv_plane_size = subsampling.subsampled_size(size).to_type<size_t>();

        Bytes buffers[] = { yuv_data->y_data(), yuv_data->u_data(), yuv_data->v_data() };
        Gfx::Size<size_t> plane_sizes[] = { y_plane_size, uv_plane_size, uv_plane_size };

        for (u32 plane = 0; plane < 3; plane++) {
            VERIFY(m_frame->linesize[plane] != 0);
            if (m_frame->linesize[plane] < 0)
                return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Reversed scanlines are not supported"sv);

            auto plane_size = plane_sizes[plane];
            auto const* source = m_frame->data[plane];
            VERIFY(source != nullptr);
            auto destination = buffers[plane];

            if (bit_depth > 8) {
                // For 10/12-bit content, normalize values to fill the full 16-bit unorm range.
                // Use bit replication: (value << shift) | (value >> inverse_shift)
                auto const shift = 16 - bit_depth;
                auto const inverse_shift = bit_depth - shift;
                auto samples_per_row = plane_size.width();
                auto source_stride = m_frame->linesize[plane];

                for (size_t row = 0; row < plane_size.height(); row++) {
                    auto const* src_row = reinterpret_cast<u16 const*>(source + (row * source_stride));
                    auto* dest_row = reinterpret_cast<u16*>(destination.data() + (row * samples_per_row * sizeof(u16)));
                    for (size_t i = 0; i < samples_per_row; i++)
                        dest_row[i] = static_cast<u16>((src_row[i] << shift) | (src_row[i] >> inverse_shift));
                }
            } else {
                auto output_line_size = plane_size.width();
                VERIFY(output_line_size <= static_cast<size_t>(m_frame->linesize[plane]));

                auto* dest_ptr = destination.data();
                for (size_t row = 0; row < plane_size.height(); row++) {
                    memcpy(dest_ptr, source, output_line_size);
                    source += m_frame->linesize[plane];
                    dest_ptr += output_line_size;
                }
            }
        }

        auto bitmap = DECODER_TRY_ALLOC(Gfx::ImmutableBitmap::create_from_yuv(move(yuv_data)));

        return DECODER_TRY_ALLOC(try_make<VideoFrame>(timestamp, duration, size, bit_depth, cicp, move(bitmap)));
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

void FFmpegVideoDecoder::flush()
{
    avcodec_flush_buffers(m_codec_context);
}

}
