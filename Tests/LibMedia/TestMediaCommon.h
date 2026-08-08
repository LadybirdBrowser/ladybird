/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Function.h>
#include <AK/NeverDestroyed.h>
#include <AK/Optional.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/Containers/Matroska/Utilities.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibTest/TestCase.h>

static inline Core::EventLoop& never_destroyed_event_loop()
{
    static NeverDestroyed<Core::EventLoop> s_event_loop;
    return *s_event_loop;
}

template<typename T>
static inline void decode_video(StringView path, size_t expected_frame_count, T create_decoder)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));
    RefPtr<Media::Matroska::TrackEntry const> video_track_entry;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track_entry = track_entry;
        return IterationDecision::Break;
    }));
    EXPECT(video_track_entry);

    auto codec_id = Media::Matroska::codec_id_from_matroska_track_entry(*video_track_entry);
    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), video_track_entry->track_number()));
    size_t frame_count = 0;
    NonnullOwnPtr<Media::VideoDecoder> decoder = create_decoder(*video_track_entry);

    auto last_timestamp = AK::Duration::min();

    while (frame_count <= expected_frame_count) {
        auto block_result = iterator.next_block();
        if (block_result.is_error() && block_result.error().category() == Media::DecoderErrorCategory::EndOfStream) {
            EXPECT_EQ(frame_count, expected_frame_count);
            return;
        }

        auto block = block_result.release_value();
        EXPECT(block.timestamp().has_value());
        auto frames = MUST(iterator.get_frames(block));
        for (auto& frame : frames) {
            auto timestamp = block.timestamp().value();
            Media::CodedFrame coded_frame { codec_id, timestamp, timestamp, block.duration().value_or(AK::Duration::zero()),
                block.only_keyframes() ? Media::FrameFlags::Keyframe : Media::FrameFlags::None,
                move(frame) };
            MUST(decoder->receive_coded_data(coded_frame));
            while (true) {
                auto metadata_result = decoder->peek_next_output({});
                if (metadata_result.is_error()) {
                    if (metadata_result.error().category() == Media::DecoderErrorCategory::NeedsMoreInput)
                        break;
                    VERIFY_NOT_REACHED();
                }
                auto metadata = metadata_result.release_value();

                auto plane_sizes = MUST(Gfx::YUVData::plane_sizes(metadata.size, metadata.bit_depth, metadata.subsampling));
                auto storage = MUST(FixedArray<u8>::create(plane_sizes.total));
                auto yuv_data = MUST(Gfx::YUVData::create(metadata.size, metadata.bit_depth, metadata.subsampling, metadata.cicp,
                    storage.span().slice(0, plane_sizes.y),
                    storage.span().slice(plane_sizes.y, plane_sizes.u),
                    storage.span().slice(plane_sizes.y + plane_sizes.u, plane_sizes.v)));
                MUST(decoder->take_next_output_into(yuv_data));

                EXPECT(last_timestamp <= metadata.timestamp);
                last_timestamp = metadata.timestamp;
            }
            frame_count++;
        }
    }

    VERIFY_NOT_REACHED();
}

static inline void decode_audio(StringView path, u32 sample_rate, u8 channel_count, size_t expected_frame_count, Optional<Audio::ChannelMap> expected_channel_map = {}, Optional<Audio::SampleSpecification> converted_sample_specification = {})
{
    auto& loop = never_destroyed_event_loop();

    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST([&] -> Media::DecoderErrorOr<NonnullRefPtr<Media::Demuxer>> {
        auto matroska_result = Media::Matroska::MatroskaDemuxer::from_stream(stream);
        if (!matroska_result.is_error())
            return matroska_result.release_value();
        return Media::FFmpeg::FFmpegDemuxer::from_stream(stream);
    }());
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());
    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));
    if (converted_sample_specification.has_value())
        TRY_OR_FAIL(producer->set_output_sample_specification(*converted_sample_specification));

    producer->set_error_handler([&](Media::DecoderError&&) {
        FAIL("An error occurred while decoding.");
    });
    producer->start();

    auto time_limit = AK::Duration::from_seconds(1);
    auto start_time = MonotonicTime::now_coarse();

    i64 last_frame = 0;
    size_t frame_count = 0;
    auto reached_end = false;

    while (true) {
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            auto const& block = *output.block;
            EXPECT(!block.is_empty());
            EXPECT_EQ(block.sample_rate(), sample_rate);
            EXPECT_EQ(block.channel_count(), channel_count);
            if (expected_channel_map.has_value())
                EXPECT_EQ(block.sample_specification().channel_map(), expected_channel_map.value());

            VERIFY(frame_count == 0 || last_frame <= block.first_frame_index());
            last_frame = block.first_frame_index() + static_cast<i64>(block.frame_count());

            frame_count += block.frame_count();
            producer->consume();
        } else if (output.status == Media::PipelineStatus::EndOfStream) {
            reached_end = true;
            break;
        }

        if (MonotonicTime::now_coarse() - start_time >= time_limit) {
            FAIL("Decoding timed out.");
            return;
        }

        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    VERIFY(reached_end);
    EXPECT_EQ(frame_count, expected_frame_count);
}

class ScriptedVideoProducer final : public Media::VideoProducer {
public:
    static NonnullRefPtr<ScriptedVideoProducer> create()
    {
        return adopt_ref(*new ScriptedVideoProducer());
    }

    void append_frame(NonnullRefPtr<Media::VideoFrame> frame)
    {
        append_output(move(frame), Media::PipelineStatus::HaveData);
    }

    void append_output(RefPtr<Media::VideoFrame> frame, Media::PipelineStatus status)
    {
        m_outputs.append({ move(frame), status });
    }

    void wake()
    {
        if (m_wake_handler)
            m_wake_handler();
    }

    virtual void start() override { }
    virtual Media::VideoProducerOutput peek() override
    {
        if (m_outputs.is_empty())
            return { nullptr, Media::PipelineStatus::Pending };
        return m_outputs.first();
    }
    virtual void consume() override { (void)m_outputs.take_first(); }
    virtual void set_wake_handler(Media::PipelineWakeHandler handler) override { m_wake_handler = move(handler); }
    virtual void seek(AK::Duration) override { }

private:
    ScriptedVideoProducer() = default;

    Vector<Media::VideoProducerOutput> m_outputs;
    Media::PipelineWakeHandler m_wake_handler;
};
