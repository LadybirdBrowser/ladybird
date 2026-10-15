/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibMedia/Containers/FLAC/FLACDemuxer.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#include "TestMediaCommon.h"
#include "TransientlyAbortingStream.h"

namespace {

TEST_SETUP
{
    Media::install_video_decoders({ Media::hardware_video_decoders(), Media::ffmpeg_video_decoders() });
    Media::install_audio_decoders({ Media::audio_decoders() });
}

// tone.flac: 48 kHz stereo, 84 frames of 4608 samples with a last frame of 1536, no seek table.
constexpr u32 TONE_SAMPLE_RATE = 48000;
constexpr u64 TONE_BLOCK_SIZE = 4608;
constexpr u64 TONE_SAMPLE_COUNT = 384000;
constexpr size_t TONE_FRAME_COUNT = 84;

// tone_8000_mono_seekpoints.flac: 8 kHz mono, frames of 4096 samples, a seek point every second.
constexpr u32 SEEKPOINTS_SAMPLE_RATE = 8000;
constexpr u64 SEEKPOINTS_BLOCK_SIZE = 4096;

ByteBuffer read_fixture(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(file->read_until_eof());
}

Media::DecoderErrorOr<NonnullRefPtr<Media::Demuxer>> demuxer_for(ByteBuffer const& data)
{
    return Media::FLAC::FLACDemuxer::from_stream(Media::IncrementallyPopulatedStream::create_from_buffer(data));
}

Media::Track audio_track(Media::Demuxer& demuxer)
{
    auto track = MUST(demuxer.get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    MUST(demuxer.create_context_for_track(*track));
    return *track;
}

AK::Duration time_of_sample(u64 sample, u32 sample_rate = TONE_SAMPLE_RATE)
{
    return AK::Duration::from_time_units(static_cast<i64>(sample), 1, sample_rate);
}

struct FrameIdentity {
    AK::Duration timestamp;
    size_t byte_count { 0 };
    bool operator==(FrameIdentity const&) const = default;
};

Vector<FrameIdentity> read_every_frame(Media::Demuxer& demuxer, Media::Track const& track)
{
    Vector<FrameIdentity> frames;
    while (true) {
        auto frame = demuxer.get_next_sample_for_track(track);
        if (frame.is_error()) {
            EXPECT_EQ(frame.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        frames.append({ frame.value().presentation_timestamp(), frame.value().data().size() });
    }
    return frames;
}

Vector<Media::FLAC::FrameHeader> frame_headers_of(ByteBuffer const& data)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(data);
    auto reader = MUST(Media::FLAC::Reader::from_stream(stream->create_cursor()));
    Media::FLAC::FrameIterator iterator { stream->create_cursor(), reader.frame_sync(), reader.first_frame_position(), Media::FLAC::FrameIterator::Resynchronize::No };
    Vector<Media::FLAC::FrameHeader> headers;
    while (true) {
        auto frame = iterator.read_next_frame();
        if (frame.is_error())
            break;
        headers.append(frame.value().header);
    }
    return headers;
}

TEST_CASE(demuxer_describes_the_only_track_of_the_stream)
{
    auto demuxer = MUST(demuxer_for(read_fixture("buffered-ranges/tone.flac"sv)));

    EXPECT(MUST(demuxer->get_tracks_for_type(Media::TrackType::Video)).is_empty());

    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.sample_rate(), TONE_SAMPLE_RATE);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.channel_map(), Audio::ChannelMap::stereo());
    EXPECT_EQ(tracks[0].parsed_codec().codec_id(), Media::CodecID::FLAC);

    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_seconds(8));
}

TEST_CASE(demuxer_reads_every_frame_with_its_sample_number)
{
    auto demuxer = MUST(demuxer_for(read_fixture("buffered-ranges/tone.flac"sv)));
    auto track = audio_track(*demuxer);

    u64 expected_sample = 0;
    size_t frame_count = 0;
    while (true) {
        auto frame = demuxer->get_next_sample_for_track(track);
        if (frame.is_error()) {
            EXPECT_EQ(frame.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        EXPECT_EQ(frame.value().presentation_timestamp(), time_of_sample(expected_sample));
        EXPECT(frame.value().is_keyframe());
        // STREAMINFO states the frame sizes of this stream as 478 to 1303 bytes.
        EXPECT(frame.value().data().size() >= 478 && frame.value().data().size() <= 1303);

        auto const& configuration = frame.value().new_codec_configuration();
        EXPECT_EQ(configuration.has_value(), frame_count == 0);
        if (configuration.has_value()) {
            EXPECT_EQ(configuration->size(), 42u);
            EXPECT(configuration->trim(4) == "fLaC"sv.bytes());
        }

        expected_sample += frame.value().duration().to_time_units(1, TONE_SAMPLE_RATE);
        frame_count++;
    }

    EXPECT_EQ(frame_count, TONE_FRAME_COUNT);
    EXPECT_EQ(expected_sample, TONE_SAMPLE_COUNT);
}

TEST_CASE(demuxer_seeks_to_the_frame_holding_a_timestamp)
{
    auto demuxer = MUST(demuxer_for(read_fixture("buffered-ranges/tone.flac"sv)));
    auto track = audio_track(*demuxer);

    auto expect_seek_to_land_on_frame = [&](AK::Duration target, u64 frame_index) {
        EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, target)), Media::DemuxerSeekResult::MovedPosition);
        auto frame = MUST(demuxer->get_next_sample_for_track(track));
        EXPECT_EQ(frame.presentation_timestamp(), time_of_sample(frame_index * TONE_BLOCK_SIZE));
        EXPECT(frame.presentation_timestamp() <= target);
        EXPECT(frame.presentation_timestamp() + frame.duration() > target);
    };
    expect_seek_to_land_on_frame(AK::Duration::from_milliseconds(3'500), 36);
    expect_seek_to_land_on_frame(AK::Duration::from_milliseconds(7'990), TONE_FRAME_COUNT - 1);
    expect_seek_to_land_on_frame(AK::Duration::from_milliseconds(96), 1);

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(10)));
    EXPECT_EQ(demuxer->get_next_sample_for_track(track).error().category(), Media::DecoderErrorCategory::EndOfStream);

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::zero(), Media::DemuxerSeekOptions::NeedCodecConfiguration));
    auto first_frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(first_frame.presentation_timestamp(), AK::Duration::zero());
    EXPECT(first_frame.new_codec_configuration().has_value());
}

TEST_CASE(demuxer_seeks_through_the_seek_table)
{
    auto demuxer = MUST(demuxer_for(read_fixture("FLAC/tone_8000_mono_seekpoints.flac"sv)));
    auto track = audio_track(*demuxer);
    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_seconds(5));

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(3'200)));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), time_of_sample(6 * SEEKPOINTS_BLOCK_SIZE, SEEKPOINTS_SAMPLE_RATE));

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(4'900)));
    auto last_frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(last_frame.presentation_timestamp(), time_of_sample(9 * SEEKPOINTS_BLOCK_SIZE, SEEKPOINTS_SAMPLE_RATE));
    EXPECT_EQ(last_frame.presentation_timestamp() + last_frame.duration(), AK::Duration::from_seconds(5));
}

// An aborted read is retried by the next call, which delivers the frame the aborted read was after, so a pass
// interrupted anywhere delivers the same frames as one that was not.
TEST_CASE(demuxer_resumes_an_aborted_read_with_the_frame_it_was_reading)
{
    auto file_data = read_fixture("buffered-ranges/tone.flac"sv);

    Vector<FrameIdentity> expected_frames;
    {
        auto demuxer = MUST(demuxer_for(file_data));
        auto track = audio_track(*demuxer);
        expected_frames = read_every_frame(*demuxer, track);
    }

    // The data ends inside a frame, so the read that blocks has part of its frame.
    auto initial_chunk_size = (file_data.size() / 3) + 1000;
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(file_data.size());
    stream->add_chunk_at(0, file_data.bytes().trim(initial_chunk_size));

    IGNORE_USE_IN_ESCAPING_LAMBDA auto demuxer = MUST(Media::FLAC::FLACDemuxer::from_stream(stream));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto track = audio_track(*demuxer);

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_was_aborted { false };
    demuxer->set_read_blocked_change_handler_for_track(track, [&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<FrameIdentity> actual_frames;
    auto reader_thread = Threading::Thread::construct("TestReader"sv, [&]() -> intptr_t {
        while (true) {
            auto frame = demuxer->get_next_sample_for_track(track);
            if (frame.is_error()) {
                EXPECT_EQ(frame.error().category(), Media::DecoderErrorCategory::Aborted);
                break;
            }
            actual_frames.append({ frame.value().presentation_timestamp(), frame.value().data().size() });
        }
        // Like a producer, the reading thread allows reads again before it reads on.
        demuxer->reset_blocking_reads_aborted_for_track(track);
        read_was_aborted = true;
        actual_frames.extend(read_every_frame(*demuxer, track));
        return 0;
    });
    reader_thread->start();

    while (!read_blocked.load()) { }
    demuxer->set_blocking_reads_aborted_for_track(track);
    while (!read_was_aborted.load()) { }

    stream->add_chunk_at(initial_chunk_size, file_data.bytes().slice(initial_chunk_size));
    stream->close();
    MUST(reader_thread->join());

    EXPECT_EQ(actual_frames.size(), expected_frames.size());
    for (size_t index = 0; index < min(actual_frames.size(), expected_frames.size()); index++) {
        if (actual_frames[index] != expected_frames[index]) {
            FAIL(ByteString::formatted("Frame {} differs after the abort: {}ms/{} bytes instead of {}ms/{} bytes", index, actual_frames[index].timestamp.to_milliseconds(), actual_frames[index].byte_count, expected_frames[index].timestamp.to_milliseconds(), expected_frames[index].byte_count));
            break;
        }
    }
}

// An abort that is reset before the reading thread looks at it still has to leave the frame it cut short to be read.
TEST_CASE(demuxer_resumes_after_a_transient_abort)
{
    auto file_data = read_fixture("buffered-ranges/tone.flac"sv);
    auto headers = frame_headers_of(file_data);
    VERIFY(headers.size() > 40);

    Vector<FrameIdentity> expected_frames;
    {
        auto demuxer = MUST(demuxer_for(file_data));
        auto track = audio_track(*demuxer);
        expected_frames = read_every_frame(*demuxer, track);
    }

    auto stream = TransientlyAbortingStream::create(file_data, headers[40].position);
    auto demuxer = MUST(Media::FLAC::FLACDemuxer::from_stream(stream));
    auto track = audio_track(*demuxer);

    Vector<FrameIdentity> actual_frames;
    size_t aborted_read_count = 0;
    while (true) {
        auto frame = demuxer->get_next_sample_for_track(track);
        if (frame.is_error()) {
            if (frame.error().category() != Media::DecoderErrorCategory::Aborted) {
                EXPECT_EQ(frame.error().category(), Media::DecoderErrorCategory::EndOfStream);
                break;
            }
            aborted_read_count++;
            continue;
        }
        actual_frames.append({ frame.value().presentation_timestamp(), frame.value().data().size() });
    }
    EXPECT(stream->has_aborted_a_read());
    EXPECT_EQ(aborted_read_count, 1u);
    EXPECT_EQ(actual_frames, expected_frames);
}

// A frame whose body is damaged is still delivered, since the frame after it is where its header says. A frame whose
// header is damaged is lost, and the frame before it runs on to the next header.
TEST_CASE(demuxer_reads_on_past_damaged_frames)
{
    auto file_data = read_fixture("buffered-ranges/tone.flac"sv);
    auto headers = frame_headers_of(file_data);
    EXPECT_EQ(headers.size(), TONE_FRAME_COUNT);

    auto damaged_body_start = headers[10].position + headers[10].byte_size + 50;
    file_data.bytes().slice(damaged_body_start, 100).fill(0);
    file_data.bytes().slice(headers[20].position, 4).fill(0);

    auto demuxer = MUST(demuxer_for(file_data));
    auto track = audio_track(*demuxer);
    auto frames = read_every_frame(*demuxer, track);

    EXPECT_EQ(frames.size(), TONE_FRAME_COUNT - 1);
    size_t index = 0;
    for (u64 frame_index = 0; frame_index < TONE_FRAME_COUNT; frame_index++) {
        if (frame_index == 20)
            continue;
        if (index < frames.size())
            EXPECT_EQ(frames[index].timestamp, time_of_sample(frame_index * TONE_BLOCK_SIZE));
        index++;
    }
}

TEST_CASE(demuxer_skips_the_tags_around_a_stream)
{
    auto plain_data = read_fixture("buffered-ranges/tone.flac"sv);
    Vector<FrameIdentity> expected_frames;
    {
        auto demuxer = MUST(demuxer_for(plain_data));
        auto track = audio_track(*demuxer);
        expected_frames = read_every_frame(*demuxer, track);
    }

    // An ID3v2 tag of 100 bytes ahead of the stream, and an ID3v1 tag after it.
    ByteBuffer tagged_data;
    MUST(tagged_data.try_append(Array<u8, 10> { 'I', 'D', '3', 3, 0, 0, 0, 0, 0, 100 }.span()));
    MUST(tagged_data.try_resize(tagged_data.size() + 100));
    MUST(tagged_data.try_append(plain_data.bytes()));
    MUST(tagged_data.try_append("TAG"sv.bytes()));
    MUST(tagged_data.try_resize(tagged_data.size() + 125));

    auto demuxer = MUST(demuxer_for(tagged_data));
    auto track = audio_track(*demuxer);
    EXPECT_EQ(read_every_frame(*demuxer, track), expected_frames);
}

TEST_CASE(demuxer_declines_what_it_cannot_read)
{
    auto not_flac = demuxer_for(MUST(ByteBuffer::create_zeroed(4096)));
    EXPECT_EQ(not_flac.error().category(), Media::DecoderErrorCategory::UnrecognizedFormat);

    ByteBuffer padding_first;
    MUST(padding_first.try_append("fLaC"sv.bytes()));
    MUST(padding_first.try_append(Array<u8, 4> { 0x81, 0, 0, 10 }.span()));
    MUST(padding_first.try_resize(padding_first.size() + 10));
    EXPECT_EQ(demuxer_for(padding_first).error().category(), Media::DecoderErrorCategory::Corrupted);

    ByteBuffer marker_only;
    MUST(marker_only.try_append("fLaC"sv.bytes()));
    EXPECT(demuxer_for(marker_only).is_error());
}

TEST_CASE(demuxer_grows_the_duration_of_a_stream_that_does_not_state_its_length)
{
    auto file_data = read_fixture("buffered-ranges/tone.flac"sv);
    // The total sample count is the 36 bits that end STREAMINFO's fixed fields, 8 bytes into the stream.
    file_data[8 + 13] &= 0xF0;
    file_data.bytes().slice(8 + 14, 4).fill(0);

    auto& loop = never_destroyed_event_loop();
    auto demuxer = MUST(demuxer_for(file_data));
    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::zero());

    demuxer->set_scan_state_change_handler([](auto const&) { });
    auto deadline_expired = false;
    auto deadline_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(1), [&] { deadline_expired = true; });
    deadline_timer->start();
    loop.spin_until([&] { return demuxer->scan_state().duration == AK::Duration::from_seconds(8) || deadline_expired; });
    demuxer->set_scan_state_change_handler(nullptr);
    EXPECT_EQ(demuxer->scan_state().duration, AK::Duration::from_seconds(8));

    auto track = audio_track(*demuxer);
    EXPECT_EQ(read_every_frame(*demuxer, track).size(), TONE_FRAME_COUNT);
}

// Over a network, a probe that lands past the target starts a request from an earlier position, so a seek into
// data that is not buffered should land before the target with as few probes as the stream's layout allows, then
// walk forward. This stream's first half is silence in tiny frames and its second half noise in large ones, so
// interpolating between the stream's ends overshoots a target in the first half by seconds. Only the metadata and
// the first frame are buffered, so that no scanned frame narrows the probing down.
TEST_CASE(demuxer_seeks_into_unbuffered_data_with_few_requests)
{
    auto file_data = read_fixture("FLAC/silence_then_noise_8000_mono.flac"sv);
    auto& loop = never_destroyed_event_loop();

    constexpr size_t head_size = 100;
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(file_data.size());
    stream->add_chunk_at(0, file_data.bytes().trim(head_size));

    // Serve the way a server does once asked: each request starts delivery at its offset, delivery goes on from
    // there a slice at a time, and a stopped request ends it.
    Vector<u64> requested_offsets;
    Optional<size_t> serving_position;
    stream->set_data_request_callback([&](Optional<u64> offset) {
        serving_position = offset.map([](u64 value) { return static_cast<size_t>(value); });
        if (offset.has_value())
            requested_offsets.append(*offset);
    });
    auto server = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [&] {
        if (!serving_position.has_value() || *serving_position >= file_data.size())
            return;
        auto slice_size = min<size_t>(2 * KiB, file_data.size() - *serving_position);
        stream->add_chunk_at(*serving_position, file_data.bytes().slice(*serving_position, slice_size));
        *serving_position += slice_size;
    });
    server->start();

    IGNORE_USE_IN_ESCAPING_LAMBDA auto demuxer = MUST(Media::FLAC::FLACDemuxer::from_stream(stream));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto track = audio_track(*demuxer);

    // The player stopped the fetch once its buffer was full and lets it go again for the seek, so every read of
    // data that is not buffered asks for a request from its own position.
    stream->set_may_idle(true);
    stream->set_may_idle(false);

    // 2.5 seconds falls in frame 4, well inside the silence.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto target = AK::Duration::from_milliseconds(2'500);
    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Duration landed_timestamp;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> seek_finished { false };
    auto seeker_thread = Threading::Thread::construct("TestSeeker"sv, [&]() -> intptr_t {
        MUST(demuxer->seek_to_most_recent_keyframe(track, target));
        landed_timestamp = MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp();
        seek_finished = true;
        return 0;
    });
    seeker_thread->start();

    auto deadline_expired = false;
    auto deadline_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { deadline_expired = true; });
    deadline_timer->start();
    loop.spin_until([&] { return seek_finished.load() || deadline_expired; });
    if (deadline_expired)
        FAIL(ByteString::formatted("The seek did not finish: {} requests, serving at {}", requested_offsets.size(), serving_position));
    server->stop();
    stream->set_data_request_callback(nullptr);
    if (!deadline_expired)
        MUST(seeker_thread->join());

    EXPECT_EQ(landed_timestamp, time_of_sample(4 * SEEKPOINTS_BLOCK_SIZE, SEEKPOINTS_SAMPLE_RATE));
    // One probe overshoots into the noise, and one step back by the noise's byte rate lands at the head.
    EXPECT(requested_offsets.size() <= 2);
}

// A header that nothing follows within a frame's reach was not a header, so the stream ends rather than delivering
// everything after it as one frame.
TEST_CASE(demuxer_gives_up_on_a_frame_that_never_ends)
{
    auto file_data = read_fixture("buffered-ranges/tone.flac"sv);
    auto first_header = frame_headers_of(file_data)[0];

    auto headerless_data = MUST(ByteBuffer::copy(file_data.bytes().trim(first_header.position + first_header.byte_size)));
    MUST(headerless_data.try_resize(headerless_data.size() + (5 * MiB), ByteBuffer::ZeroFillNewElements::Yes));

    auto demuxer = MUST(demuxer_for(headerless_data));
    auto track = audio_track(*demuxer);
    EXPECT(read_every_frame(*demuxer, track).is_empty());
}

TEST_CASE(demuxer_reads_no_more_of_a_seek_table_than_it_can_use)
{
    auto file_data = read_fixture("buffered-ranges/tone.flac"sv);
    auto headers = frame_headers_of(file_data);
    auto first_frame_position = headers[0].position;

    // The stream's STREAMINFO, then a seek table of 70,000 points of which only the first 84 are real, then its frames.
    constexpr size_t seek_point_count = 70'000;
    ByteBuffer tabled_data;
    MUST(tabled_data.try_append(file_data.bytes().trim(4 + 4 + 34)));
    tabled_data[4] = 0x00;
    u32 table_size = seek_point_count * 18;
    MUST(tabled_data.try_append(Array<u8, 4> { 0x83, static_cast<u8>(table_size >> 16), static_cast<u8>(table_size >> 8), static_cast<u8>(table_size) }.span()));
    for (size_t index = 0; index < seek_point_count; index++) {
        u64 sample_number = NumericLimits<u64>::max();
        u64 byte_offset = 0;
        if (index < headers.size()) {
            sample_number = headers[index].sample_number;
            byte_offset = headers[index].position - first_frame_position;
        }
        MUST(tabled_data.try_append(Array<u8, 18> { static_cast<u8>(sample_number >> 56), static_cast<u8>(sample_number >> 48), static_cast<u8>(sample_number >> 40), static_cast<u8>(sample_number >> 32), static_cast<u8>(sample_number >> 24), static_cast<u8>(sample_number >> 16), static_cast<u8>(sample_number >> 8), static_cast<u8>(sample_number), static_cast<u8>(byte_offset >> 56), static_cast<u8>(byte_offset >> 48), static_cast<u8>(byte_offset >> 40), static_cast<u8>(byte_offset >> 32), static_cast<u8>(byte_offset >> 24), static_cast<u8>(byte_offset >> 16), static_cast<u8>(byte_offset >> 8), static_cast<u8>(byte_offset), 0x12, 0x00 }.span()));
    }
    MUST(tabled_data.try_append(file_data.bytes().slice(first_frame_position)));

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(tabled_data);
    auto reader = MUST(Media::FLAC::Reader::from_stream(stream->create_cursor()));
    EXPECT_EQ(reader.seek_points().size(), headers.size());

    auto demuxer = MUST(Media::FLAC::FLACDemuxer::from_stream(stream));
    auto track = audio_track(*demuxer);
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(3'500)));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), time_of_sample(36 * TONE_BLOCK_SIZE));
}

TEST_CASE(demuxer_feeds_a_decoder_to_the_end)
{
    decode_audio("buffered-ranges/tone.flac"sv, TONE_SAMPLE_RATE, 2, TONE_SAMPLE_COUNT);
    decode_audio("FLAC/tone_8000_mono_seekpoints.flac"sv, SEEKPOINTS_SAMPLE_RATE, 1, 40000);
}

}
