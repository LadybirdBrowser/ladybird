/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Optional.h>
#include <LibAudio/ChannelMap.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/Containers/Matroska/Utilities.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibMediaCodecs/VideoFrame.h>
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
            MUST(decoder->receive_coded_data(coded_frame, Media::DecodeIntent::Output));
            while (true) {
                auto decoded_frame_result = decoder->take_next_output({});
                if (decoded_frame_result.is_error()) {
                    if (decoded_frame_result.error().category() == Media::DecoderErrorCategory::NeedsMoreInput)
                        break;
                    VERIFY_NOT_REACHED();
                }
                auto decoded_frame = decoded_frame_result.release_value();

                EXPECT(last_timestamp <= decoded_frame->timestamp());
                last_timestamp = decoded_frame->timestamp();
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
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());
    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));
    if (converted_sample_specification.has_value())
        TRY_OR_FAIL(producer->set_output_sample_specification(*converted_sample_specification));

    producer->set_error_handler([&](Media::DecoderError&&) {
        FAIL("An error occurred while decoding.");
    });
    producer->start();

    auto time_limit = AK::Duration::from_seconds(10);
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
        MutexLocker locker { m_mutex };
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
        MutexLocker locker { m_mutex };
        if (m_outputs.is_empty())
            return { nullptr, Media::PipelineStatus::Pending };
        return m_outputs.first();
    }
    virtual void consume() override
    {
        MutexLocker locker { m_mutex };
        (void)m_outputs.take_first();
    }
    virtual void set_wake_handler(Media::PipelineWakeHandler handler) override { m_wake_handler = move(handler); }
    virtual void seek(AK::Duration, Media::InvalidateHeldData = Media::InvalidateHeldData::No) override { m_seek_count++; }

    size_t seek_count() const { return m_seek_count; }

private:
    ScriptedVideoProducer() = default;

    Mutex m_mutex;
    Vector<Media::VideoProducerOutput> m_outputs;
    size_t m_seek_count { 0 };
    Media::PipelineWakeHandler m_wake_handler;
};

static inline NonnullRefPtr<Media::IncrementallyPopulatedStream> load_test_file(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
}

static inline NonnullRefPtr<Media::Demuxer> create_demuxer(NonnullRefPtr<Media::IncrementallyPopulatedStream> const& stream)
{
    return MUST(Media::create_demuxer(stream));
}

template<typename Condition>
static inline bool pump_until(Core::EventLoop& loop, Condition condition)
{
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(10);
    while (MonotonicTime::now_coarse() < deadline) {
        if (condition())
            return true;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    return false;
}

static constexpr AK::Duration SHORT_AUTO_SUSPEND_TIMEOUT = AK::Duration::from_milliseconds(50);

static inline void sleep_past_the_idle_timeout()
{
    MUST(Core::System::sleep_ms(static_cast<u32>(SHORT_AUTO_SUSPEND_TIMEOUT.to_milliseconds() * 4)));
}

// A stalled test thread can leave the producer idle long enough to suspend before it is ever seen with data.
static inline bool has_data_or_already_suspended(Media::PipelineStatus status)
{
    return status == Media::PipelineStatus::HaveData || status == Media::PipelineStatus::Suspended;
}

// A stalled test thread can leave the producer idle long enough to suspend again before it peeks, and a consumer that
// sees a suspension must seek again. These resume from a seek as such a consumer would.
// Returns the output that showed the data, since the producer can suspend again and drop it before another peek.
// seek_count counts the seeks issued, the first included.
template<typename Producer>
static inline auto seek_to_resume_and_pump_until_woken_with_data(Core::EventLoop& loop, Producer& producer, AK::Duration timestamp, size_t& seek_count)
{
    Optional<decltype(producer.peek())> output_with_data;
    bool woken = false;
    producer.set_wake_handler([&] { woken = true; });
    producer.seek(timestamp);
    seek_count = 1;
    pump_until(loop, [&] {
        if (!woken)
            return false;
        woken = false;
        auto output = producer.peek();
        if (output.status == Media::PipelineStatus::Suspended) {
            producer.seek(timestamp);
            seek_count++;
            return false;
        }
        if (output.status != Media::PipelineStatus::HaveData)
            return false;
        output_with_data = move(output);
        return true;
    });
    producer.set_wake_handler(nullptr);
    return output_with_data;
}

template<typename Producer>
static inline auto seek_to_resume_and_pump_until_woken_with_data(Core::EventLoop& loop, Producer& producer, AK::Duration timestamp)
{
    size_t seek_count = 0;
    return seek_to_resume_and_pump_until_woken_with_data(loop, producer, timestamp, seek_count);
}

struct ExpectedScenePeak {
    i64 milliseconds;
    Optional<double> luminance;
};

// The frames of these files were given scene peaks in decoding order, so a peak that does not follow its frame through
// reordering lands on a frame expecting another.
static constexpr Array<ExpectedScenePeak, 6> hevc_hdr10_plus_in_sei_scene_peaks { {
    { 0, 100 },
    { 33, 400 },
    { 67, 500 },
    { 100, 300 },
    { 133, 600 },
    { 167, 200 },
} };
// Each frame's RPU takes another path through the syntax: a polynomial mapping, floating point coefficients, a reused
// mapping with compressed display management, nonlinear quantization, no display management at all, and MMR.
static constexpr Array<ExpectedScenePeak, 6> hevc_dolby_vision_rpus_scene_peaks { {
    { 0, 100.1 },
    { 33, 300.33 },
    { 67, 599.56 },
    { 100, 500.44 },
    { 133, {} },
    { 167, 399.72 },
} };

// Each expected frame has to be checked exactly once, whatever order the frames come in.
class ScenePeakChecker {
public:
    explicit ScenePeakChecker(Span<ExpectedScenePeak const> expected_scene_peaks)
        : m_expected_scene_peaks(expected_scene_peaks)
    {
        m_checked.resize(expected_scene_peaks.size());
    }

    void check(AK::Duration timestamp, Optional<float> maximum_scene_luminance)
    {
        Optional<size_t> expected_index;
        for (size_t index = 0; index < m_expected_scene_peaks.size(); index++) {
            if (m_expected_scene_peaks[index].milliseconds == timestamp.to_milliseconds())
                expected_index = index;
        }
        EXPECT(expected_index.has_value());
        if (!expected_index.has_value())
            return;
        EXPECT(!m_checked[expected_index.value()]);
        m_checked[expected_index.value()] = true;

        auto const& expected = m_expected_scene_peaks[expected_index.value()];
        EXPECT_EQ(maximum_scene_luminance.has_value(), expected.luminance.has_value());
        if (!maximum_scene_luminance.has_value() || !expected.luminance.has_value())
            return;
        EXPECT(isfinite(maximum_scene_luminance.value()));
        EXPECT_APPROXIMATE_WITH_ERROR(maximum_scene_luminance.value(), expected.luminance.value(), 0.5);
    }

    void expect_every_frame_checked() const
    {
        for (auto checked : m_checked)
            EXPECT(checked);
    }

private:
    Span<ExpectedScenePeak const> m_expected_scene_peaks;
    Vector<bool> m_checked;
};
