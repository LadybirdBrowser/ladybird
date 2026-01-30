/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioDecoding.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>

namespace Media {

static DecoderErrorOr<NonnullRefPtr<Demuxer>> create_demuxer_for_stream(NonnullRefPtr<IncrementallyPopulatedStream> const& stream)
{
    if (Matroska::Reader::is_matroska_or_webm(stream->create_cursor()))
        return Matroska::MatroskaDemuxer::from_stream(stream);
    return FFmpeg::FFmpegDemuxer::from_stream(stream);
}

DecoderErrorOr<DecodedAudioData> decode_first_audio_track_to_pcm_f32(ByteBuffer&& encoded_data, Optional<u32> target_sample_rate)
{
    return decode_first_audio_track_to_pcm_f32(move(encoded_data), target_sample_rate, {});
}

DecoderErrorOr<DecodedAudioData> decode_first_audio_track_to_pcm_f32(ByteBuffer&& encoded_data, Optional<u32> target_sample_rate, Function<bool()> is_cancelled)
{
    auto check_cancelled = [&]() -> DecoderErrorOr<void> {
        if (is_cancelled && is_cancelled())
            return DecoderError::with_description(DecoderErrorCategory::Aborted, "Decoding cancelled"sv);
        return {};
    };

    TRY(check_cancelled());

    auto stream = IncrementallyPopulatedStream::create_from_buffer(move(encoded_data));
    TRY(check_cancelled());
    auto demuxer = TRY(create_demuxer_for_stream(stream));
    TRY(check_cancelled());

    auto tracks = TRY(demuxer->get_tracks_for_type(TrackType::Audio));
    if (tracks.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "No audio tracks found"sv);

    auto const& track = tracks.first();

    auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
    auto const& input_sample_specification = track.audio_data().sample_specification;
    auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(track));

    auto decoder = DECODER_TRY_ALLOC(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, input_sample_specification, codec_initialization_data));
    auto converter = DECODER_TRY_ALLOC(FFmpeg::FFmpegAudioConverter::try_create());

    TRY(check_cancelled());

    auto stream_cursor = stream->create_cursor();
    TRY(demuxer->create_context_for_track(track));

    DecodedAudioData decoded;
    bool converter_configured = false;

    auto append_block = [&](AudioBlock& block) -> DecoderErrorOr<void> {
        TRY(check_cancelled());

        if (!converter_configured) {
            // Prefer the first decoded block's channel map over container metadata.
            auto const& decoded_input_sample_specification = block.sample_specification();
            Audio::SampleSpecification output_sample_specification = decoded_input_sample_specification;
            if (target_sample_rate.has_value() && *target_sample_rate > 0 && decoded_input_sample_specification.sample_rate() != *target_sample_rate)
                output_sample_specification = Audio::SampleSpecification(*target_sample_rate, decoded_input_sample_specification.channel_map());

            if (!output_sample_specification.is_valid())
                return DecoderError::with_description(DecoderErrorCategory::Invalid, "Invalid output sample specification"sv);

            auto result = converter->set_output_sample_specification(output_sample_specification);
            if (result.is_error())
                return DecoderError::with_description(DecoderErrorCategory::NotImplemented, result.error().string_literal());

            decoded.sample_specification = output_sample_specification;
            converter_configured = true;
        }

        auto convert_result = converter->convert(block);
        if (convert_result.is_error())
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, convert_result.error().string_literal());

        auto data = block.data().span();
        Checked<size_t> new_size = decoded.interleaved_f32_samples.size();
        new_size += data.size();
        if (new_size.has_overflow())
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Decoded audio is too large"sv);

        auto append_result = decoded.interleaved_f32_samples.try_append(data.data(), data.size());
        if (append_result.is_error())
            return DecoderError::with_description(DecoderErrorCategory::Memory, append_result.error().string_literal());

        return {};
    };

    auto drain_decoder = [&]() -> DecoderErrorOr<void> {
        for (;;) {
            TRY(check_cancelled());
            AudioBlock block;
            auto block_result = decoder->write_next_block(block);
            if (block_result.is_error()) {
                if (block_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                    return {};
                if (block_result.error().category() == DecoderErrorCategory::EndOfStream)
                    return {};
                return block_result.release_error();
            }
            TRY(append_block(block));
        }
    };

    for (;;) {
        TRY(check_cancelled());
        auto coded_frame_result = demuxer->get_next_sample_for_track(track);
        if (coded_frame_result.is_error()) {
            if (coded_frame_result.error().category() == DecoderErrorCategory::EndOfStream)
                break;
            return coded_frame_result.release_error();
        }
        auto coded_frame = coded_frame_result.release_value();

        TRY(decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.data().bytes()));
        TRY(drain_decoder());
    }

    TRY(check_cancelled());
    decoder->signal_end_of_stream();
    TRY(drain_decoder());

    TRY(check_cancelled());

    if (!decoded.sample_specification.is_valid() || decoded.sample_specification.channel_count() == 0 || decoded.interleaved_f32_samples.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Decoded audio is empty"sv);

    return decoded;
}

}
