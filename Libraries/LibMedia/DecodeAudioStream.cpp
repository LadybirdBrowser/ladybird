/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecodeAudioStream.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Track.h>

namespace Media {

// Materializing an entire stream can require a large amount of memory for long inputs, so all sample storage is
// allocated fallibly and any failure is reported as a decoder error instead of crashing the process.
static DecoderErrorOr<void> append_block_samples(AudioBlock const& block, DecodedAudioData& data)
{
    if (block.frame_count() == 0)
        return {};

    if (!data.sample_specification.is_valid()) {
        data.sample_specification = block.sample_specification();
        if (data.channels.try_resize(block.channel_count()).is_error())
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Unable to allocate channel storage"sv);
    }
    VERIFY(block.channel_count() == data.channels.size());

    Checked<size_t> new_frame_count { data.channels[0].size() };
    new_frame_count += block.frame_count();
    if (new_frame_count.has_overflow())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Decoded audio stream is too large"sv);

    for (size_t channel = 0; channel < data.channels.size(); channel++) {
        auto channel_data = block.channel_data(channel);
        if (data.channels[channel].try_append(channel_data.data(), channel_data.size()).is_error())
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Unable to allocate sample storage"sv);
    }
    return {};
}

// Demuxes and decodes the preferred audio track of the provided stream in its entirety, converting every decoded block
// to a single sample specification: the requested output sample rate (or the stream's native rate) combined with the
// stream's native channel map. Routing all blocks through the converter also makes streams whose sample specification
// changes mid-stream come out uniform.
DecoderErrorOr<DecodedAudioData> decode_entire_audio_stream(NonnullRefPtr<MediaStream> const& stream, Optional<u32> output_sample_rate)
{
    auto demuxer = TRY(PlaybackManager::create_demuxer_for_stream(stream));
    auto track = TRY(demuxer->get_preferred_track_for_type(TrackType::Audio));
    if (!track.has_value()) {
        // NB: Not all containers mark a default track; fall back to the first audio track in that case.
        auto audio_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Audio));
        if (audio_tracks.is_empty())
            return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Stream contains no audio track"sv);
        track = audio_tracks.first();
    }
    TRY(demuxer->create_context_for_track(*track));

    auto codec_id = TRY(demuxer->get_codec_id_for_track(*track));
    auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(*track));
    auto decoder = TRY(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, track->audio_data().sample_specification, codec_initialization_data));
    auto converter = DECODER_TRY_ALLOC(FFmpeg::FFmpegAudioConverter::try_create());

    DecodedAudioData data;
    Optional<Audio::SampleSpecification> target_specification;
    auto push_decoded_block = [&](AudioBlock const& block) -> DecoderErrorOr<void> {
        if (block.frame_count() == 0)
            return {};

        if (!target_specification.has_value())
            target_specification = Audio::SampleSpecification { output_sample_rate.value_or(block.sample_specification().sample_rate()), block.sample_specification().channel_map() };
        if (auto result = converter->set_output_sample_specification(*target_specification); result.is_error())
            return DecoderError::format(DecoderErrorCategory::NotImplemented, "Unable to configure sample conversion: {}", result.error().string_literal());
        if (auto result = converter->push_block(block); result.is_error())
            return DecoderError::format(DecoderErrorCategory::NotImplemented, "Sample specification conversion failed: {}", result.error().string_literal());

        return {};
    };

    auto retrieve_next_converted_block = [&](AudioBlock& block) -> DecoderErrorOr<void> {
        while (true) {
            auto retrieve_result = converter->retrieve_block(block);
            if (!retrieve_result.is_error())
                return {};
            if (retrieve_result.error().category() != DecoderErrorCategory::NeedsMoreInput)
                return retrieve_result.release_error();

            auto block_result = decoder->write_next_block(block);
            if (block_result.is_error()) {
                if (block_result.error().category() != DecoderErrorCategory::EndOfStream)
                    return block_result.release_error();
                converter->signal_end_of_stream();
                continue;
            }

            TRY(push_decoded_block(block));
            block.clear();
        }
    };

    AudioBlock block;
    auto end_of_stream_reached = false;
    while (!end_of_stream_reached) {
        auto sample_result = demuxer->get_next_sample_for_track(*track);
        if (sample_result.is_error()) {
            if (sample_result.error().category() != DecoderErrorCategory::EndOfStream)
                return sample_result.release_error();
            decoder->signal_end_of_stream();
        } else {
            auto sample = sample_result.release_value();
            TRY(decoder->receive_coded_data(sample.timestamp(), sample.data()));
        }

        while (true) {
            auto block_result = retrieve_next_converted_block(block);
            if (block_result.is_error()) {
                if (block_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                    break;
                if (block_result.error().category() == DecoderErrorCategory::EndOfStream) {
                    end_of_stream_reached = true;
                    break;
                }
                return block_result.release_error();
            }

            TRY(append_block_samples(block, data));
        }
    }

    if (!data.sample_specification.is_valid())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Audio stream contains no samples"sv);

    return data;
}

}
