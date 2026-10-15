/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibMedia/Containers/Ogg/OggDemuxer.h>
#include <LibMedia/Containers/Ogg/Reader.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#include "TestMediaCommon.h"

namespace {

TEST_SETUP
{
    (void)never_destroyed_event_loop();
    Media::install_video_decoders({ Media::hardware_video_decoders(), Media::ffmpeg_video_decoders() });
    Media::install_audio_decoders({ Media::audio_decoders() });
}

// sine_opus_1005ms.ogg: 48 kHz mono Opus, 48240 samples after a pre-skip of 312, packets of 960 samples.
constexpr i64 OPUS_PRE_SKIP = 312;
constexpr i64 OPUS_SAMPLE_COUNT = 48240;
// theora_vorbis_10fps.ogg: 64x48 Theora at 10 fps with a keyframe every 10 frames, beside 8 kHz mono Vorbis, 3 s.
constexpr size_t THEORA_FRAME_COUNT = 30;

ByteBuffer read_fixture(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(file->read_until_eof());
}

Media::DecoderErrorOr<NonnullRefPtr<Media::Demuxer>> demuxer_for(ByteBuffer const& data)
{
    return Media::Ogg::OggDemuxer::from_stream(Media::IncrementallyPopulatedStream::create_from_buffer(data));
}

Media::Track track_of_type(Media::Demuxer& demuxer, Media::TrackType type)
{
    auto track = MUST(demuxer.get_preferred_track_for_type(type));
    VERIFY(track.has_value());
    MUST(demuxer.create_context_for_track(*track));
    return *track;
}

AK::Duration time_of_sample(i64 sample, u32 sample_rate = 48000)
{
    return AK::Duration::from_time_units(sample, 1, sample_rate);
}

struct FrameIdentity {
    AK::Duration timestamp;
    size_t byte_count { 0 };
    bool operator==(FrameIdentity const&) const = default;
};

Vector<Media::CodedFrame> read_every_frame(Media::Demuxer& demuxer, Media::Track const& track)
{
    Vector<Media::CodedFrame> frames;
    while (true) {
        auto frame = demuxer.get_next_sample_for_track(track);
        if (frame.is_error()) {
            EXPECT_EQ(frame.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        frames.append(frame.release_value());
    }
    return frames;
}

Vector<FrameIdentity> identities_of(Vector<Media::CodedFrame> const& frames)
{
    Vector<FrameIdentity> identities;
    for (auto const& frame : frames)
        identities.append({ frame.presentation_timestamp(), frame.data().size() });
    return identities;
}

bool spin_until(Function<bool()> condition)
{
    auto& loop = never_destroyed_event_loop();
    auto deadline_expired = false;
    auto deadline_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { deadline_expired = true; });
    deadline_timer->start();
    loop.spin_until([&] { return condition() || deadline_expired; });
    return !deadline_expired;
}

Vector<Media::Ogg::PageHeader> page_headers(ByteBuffer const& data)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(data);
    Media::Ogg::PageIterator pages { stream->create_cursor(), 0 };
    Vector<Media::Ogg::PageHeader> headers;
    while (pages.position() < data.size())
        headers.append(MUST(pages.read_next_page()).header);
    return headers;
}

Media::Ogg::Page packet_page(u32 sequence_number, bool continues_packet, ReadonlyBytes segment_sizes)
{
    size_t payload_size = 0;
    for (auto size : segment_sizes)
        payload_size += size;
    auto payload = MUST(FixedArray<u8>::create(payload_size));
    payload.span().fill(static_cast<u8>(sequence_number));
    return {
        .header = { .flags = static_cast<u8>(continues_packet ? 1 : 0), .sequence_number = sequence_number },
        .segment_sizes = MUST(FixedArray<u8>::create(segment_sizes)),
        .payload = move(payload),
    };
}

TEST_CASE(packet_assembly_handles_empty_packets_and_continuations)
{
    Media::Ogg::PacketAssembler assembler;
    auto first_page = packet_page(0, false, Array<u8, 4> { 0, 255, 0, 255 });
    auto packets = MUST(assembler.take_completed_packets(first_page));
    EXPECT_EQ(packets.size(), 2u);
    EXPECT(packets[0].is_empty());
    EXPECT_EQ(packets[1].size(), 255u);

    auto middle_page = packet_page(1, true, Array<u8, 1> { 255 });
    EXPECT(MUST(assembler.take_completed_packets(middle_page)).is_empty());
    auto final_page = packet_page(2, true, Array<u8, 2> { 0, 1 });
    packets = MUST(assembler.take_completed_packets(final_page));
    EXPECT_EQ(packets.size(), 2u);
    EXPECT_EQ(packets[0].size(), 510u);
    EXPECT_EQ(packets[0][0], 0u);
    EXPECT_EQ(packets[0][255], 1u);
    EXPECT_EQ(packets[1].size(), 1u);
    EXPECT_EQ(packets[1][0], 2u);
}

TEST_CASE(packet_assembly_discards_a_continuation_after_a_missing_page)
{
    Media::Ogg::PacketAssembler assembler;
    EXPECT(MUST(assembler.take_completed_packets(packet_page(0, false, Array<u8, 1> { 255 }))).is_empty());
    auto packets = MUST(assembler.take_completed_packets(packet_page(2, true, Array<u8, 2> { 1, 2 })));
    EXPECT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].size(), 2u);

    assembler.reset();
    packets = MUST(assembler.take_completed_packets(packet_page(2, true, Array<u8, 2> { 1, 2 })));
    EXPECT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].size(), 2u);
}

TEST_CASE(packet_assembly_preserves_continuity_across_sequence_number_wraparound)
{
    Media::Ogg::PacketAssembler assembler;
    EXPECT(MUST(assembler.take_completed_packets(packet_page(NumericLimits<u32>::max(), false, Array<u8, 1> { 255 }))).is_empty());
    auto packets = MUST(assembler.take_completed_packets(packet_page(0, true, Array<u8, 1> { 1 })));
    EXPECT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].size(), 256u);
}

TEST_CASE(buffered_scan_resynchronizes_after_an_invalid_header_near_the_end)
{
    auto data = read_fixture("buffered-ranges/tone_vorbis.ogg"sv);
    auto headers = page_headers(data);
    auto damaged_position = headers[headers.size() - 2].position;
    VERIFY(data.size() - damaged_position < Media::Ogg::MAXIMUM_PAGE_SIZE);
    data[damaged_position] = 'X';
    auto demuxer = MUST(demuxer_for(data));
    demuxer->set_scan_state_change_handler([](auto const&) { });
    auto expected_end = time_of_sample(headers.last().granule_position, 44100);
    EXPECT(spin_until([&] { return demuxer->scan_state().buffered_ranges().highest_end_time() == expected_end; }));
}

TEST_CASE(buffered_scan_waits_for_a_partial_page_to_finish)
{
    auto data = read_fixture("buffered-ranges/tone_vorbis.ogg"sv);
    auto headers = page_headers(data);
    auto const& partial_page = headers[headers.size() - 2];
    auto expected_end = time_of_sample(headers.last().granule_position, 44100);
    for (auto split_position : { partial_page.position + 12, partial_page.position + partial_page.byte_size / 2 }) {
        auto stream = Media::IncrementallyPopulatedStream::create_empty();
        stream->add_chunk_at(0, data.bytes().trim(split_position));
        auto demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
        demuxer->set_scan_state_change_handler([](auto const&) { });
        auto preceding_page_end = time_of_sample(headers[headers.size() - 3].granule_position, 44100);
        EXPECT(spin_until([&] { return demuxer->scan_state().duration == preceding_page_end; }));

        stream->add_chunk_at(split_position, data.bytes().slice(split_position));
        stream->close();
        EXPECT(spin_until([&] { return demuxer->scan_state().duration == expected_end; }));
    }
}

TEST_CASE(buffered_scan_connects_a_previously_isolated_link)
{
    auto data = read_fixture("Ogg/chained_opus_then_vorbis.ogg"sv);
    auto headers = page_headers(data);
    auto first_link_end_page = headers[3];
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->add_chunk_at(0, data.bytes().trim(first_link_end_page.position));
    stream->add_chunk_at(first_link_end_page.end_position(), data.bytes().slice(first_link_end_page.end_position()));
    auto demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
    demuxer->set_scan_state_change_handler([](auto const&) { });
    EXPECT(spin_until([&] { return !demuxer->scan_state().buffered_ranges().is_empty(); }));

    stream->add_chunk_at(first_link_end_page.position, data.bytes().slice(first_link_end_page.position));
    stream->close();
    auto expected_end = time_of_sample(OPUS_SAMPLE_COUNT) + AK::Duration::from_seconds(1);
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == expected_end; }));
    auto track = track_of_type(*demuxer, Media::TrackType::Audio);
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(1500), Media::DemuxerSeekOptions::None));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).codec_id(), Media::CodecID::Vorbis);
}

TEST_CASE(buffered_scan_merges_concurrent_streams_discovered_separately)
{
    auto data = read_fixture("buffered-ranges/tone_vorbis.ogg"sv);
    auto first_link_headers = page_headers(data);
    auto first_link_duration = time_of_sample(first_link_headers.last().granule_position, 44100);
    auto second_link = read_fixture("Ogg/theora_vorbis_10fps.ogg"sv);
    auto second_link_headers = page_headers(second_link);
    auto missing_byte_position = data.size() + second_link_headers[0].end_position() - 1;
    MUST(data.try_append(second_link));
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->add_chunk_at(0, data.bytes().trim(missing_byte_position));
    stream->add_chunk_at(missing_byte_position + 1, data.bytes().slice(missing_byte_position + 1));
    auto demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
    demuxer->set_scan_state_change_handler([](auto const&) { });
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == first_link_duration; }));

    stream->add_chunk_at(missing_byte_position, data.bytes().slice(missing_byte_position));
    stream->close();
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == first_link_duration + AK::Duration::from_seconds(3); }));
    auto track = track_of_type(*demuxer, Media::TrackType::Audio);
    MUST(demuxer->seek_to_most_recent_keyframe(track, first_link_duration + AK::Duration::from_milliseconds(1500), Media::DemuxerSeekOptions::None));
    auto frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(frame.presentation_timestamp() >= first_link_duration);
    EXPECT(frame.presentation_timestamp() <= first_link_duration + AK::Duration::from_milliseconds(1500));
}

TEST_CASE(buffered_scan_resolves_data_seen_before_its_stream_headers)
{
    auto data = read_fixture("Ogg/sine_opus_1005ms.ogg"sv);
    auto second_link_position = data.size();
    auto second_link = read_fixture("buffered-ranges/tone_vorbis.ogg"sv);
    auto headers = page_headers(second_link);
    auto second_link_header_end = second_link_position + headers[1].end_position();
    auto tail_position = second_link_position + headers[headers.size() / 2].position;
    MUST(data.try_append(second_link));
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->add_chunk_at(0, data.bytes().trim(second_link_position));
    stream->add_chunk_at(tail_position, data.bytes().slice(tail_position));
    auto demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
    demuxer->set_scan_state_change_handler([](auto const&) { });
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == time_of_sample(OPUS_SAMPLE_COUNT); }));

    stream->add_chunk_at(second_link_position, data.bytes().slice(second_link_position, second_link_header_end - second_link_position));
    auto expected_end = time_of_sample(OPUS_SAMPLE_COUNT) + time_of_sample(headers.last().granule_position, 44100);
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == expected_end; }));
    EXPECT_EQ(demuxer->scan_state().buffered_ranges().size(), 2u);
}

TEST_CASE(buffered_scan_resumes_reading_later_link_headers)
{
    auto data = read_fixture("Ogg/chained_opus_then_vorbis.ogg"sv);
    auto headers = page_headers(data);
    auto split_position = headers[5].position + headers[5].byte_size / 2;
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->add_chunk_at(0, data.bytes().trim(split_position));
    stream->add_chunk_at(headers[6].position, data.bytes().slice(headers[6].position));
    auto demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
    demuxer->set_scan_state_change_handler([](auto const&) { });
    auto first_link_duration = time_of_sample(OPUS_SAMPLE_COUNT);
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == first_link_duration; }));

    stream->add_chunk_at(split_position, data.bytes().slice(split_position));
    stream->close();
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == first_link_duration + AK::Duration::from_seconds(1); }));
}

TEST_CASE(buffered_scan_retains_later_vorbis_configuration_after_header_eviction)
{
    auto data = read_fixture("Ogg/chained_opus_then_vorbis.ogg"sv);
    auto headers = page_headers(data);
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(data);
    auto demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
    demuxer->set_scan_state_change_handler([](auto const&) { });
    auto first_link_duration = time_of_sample(OPUS_SAMPLE_COUNT);
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == first_link_duration + AK::Duration::from_seconds(1); }));

    stream->remove_byte_range(0, headers[6].position);
    EXPECT(spin_until([&] { return !demuxer->scan_state().resource_is_complete; }));
    auto ranges = demuxer->scan_state().buffered_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    if (ranges.size() == 1) {
        EXPECT(ranges[0].start >= first_link_duration);
        EXPECT(ranges[0].start < first_link_duration + AK::Duration::from_milliseconds(50));
    }
}

TEST_CASE(demuxer_describes_every_stream_of_the_file)
{
    auto demuxer = MUST(demuxer_for(read_fixture("Ogg/theora_vorbis_10fps.ogg"sv)));

    auto video_tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Video));
    EXPECT_EQ(video_tracks.size(), 1u);
    EXPECT_EQ(video_tracks[0].parsed_codec().codec_id(), Media::CodecID::Theora);
    EXPECT_EQ(video_tracks[0].video_data().pixel_width, 64u);
    EXPECT_EQ(video_tracks[0].video_data().pixel_height, 48u);

    auto audio_tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(audio_tracks.size(), 1u);
    EXPECT_EQ(audio_tracks[0].parsed_codec().codec_id(), Media::CodecID::Vorbis);
    EXPECT_EQ(audio_tracks[0].audio_data().sample_specification.sample_rate(), 8000u);
    EXPECT_EQ(audio_tracks[0].audio_data().sample_specification.channel_count(), 1u);

    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_seconds(3));
}

// Pre-skip is dropped by the decoder, and the last page's granule position trims the last packet, so the packets
// cover exactly the stream's samples.
TEST_CASE(demuxer_leaves_opus_pre_skip_out_and_trims_the_last_packet)
{
    auto demuxer = MUST(demuxer_for(read_fixture("Ogg/sine_opus_1005ms.ogg"sv)));
    auto track = track_of_type(*demuxer, Media::TrackType::Audio);
    EXPECT_EQ(MUST(demuxer->total_duration()), time_of_sample(OPUS_SAMPLE_COUNT));

    auto frames = read_every_frame(*demuxer, track);
    VERIFY(!frames.is_empty());
    EXPECT(frames.first().new_codec_configuration().has_value());
    EXPECT_EQ(frames.first().presentation_timestamp(), AK::Duration::zero());
    EXPECT_EQ(frames.first().leading_discard(), AK::Duration::zero());
    EXPECT_EQ(frames.first().duration(), time_of_sample(960 - OPUS_PRE_SKIP));

    for (size_t i = 1; i < frames.size(); i++) {
        auto const& previous = frames[i - 1];
        EXPECT_EQ(frames[i].presentation_timestamp(), previous.presentation_timestamp() + previous.duration());
        EXPECT(!frames[i].new_codec_configuration().has_value());
    }
    auto const& last = frames.last();
    EXPECT(last.trailing_discard() > AK::Duration::zero());
    EXPECT_EQ(last.presentation_timestamp() + last.duration(), time_of_sample(OPUS_SAMPLE_COUNT));
}

TEST_CASE(demuxer_feeds_decoders_to_the_end)
{
    decode_audio("Ogg/sine_opus_1005ms.ogg"sv, 48000, 1, OPUS_SAMPLE_COUNT);
    decode_audio("buffered-ranges/tone_flac.ogg"sv, 44100, 1, 1323000);
}

TEST_CASE(demuxer_reads_theora_frames_and_their_keyframes)
{
    auto demuxer = MUST(demuxer_for(read_fixture("Ogg/theora_vorbis_10fps.ogg"sv)));
    auto track = track_of_type(*demuxer, Media::TrackType::Video);

    auto frames = read_every_frame(*demuxer, track);
    EXPECT_EQ(frames.size(), THEORA_FRAME_COUNT);
    for (size_t i = 0; i < frames.size(); i++) {
        EXPECT_EQ(frames[i].presentation_timestamp(), AK::Duration::from_milliseconds(static_cast<i64>(i) * 100));
        EXPECT_EQ(frames[i].is_keyframe(), i % 10 == 0);
    }
}

TEST_CASE(theora_frames_decode_to_the_end)
{
    auto& loop = never_destroyed_event_loop();
    auto demuxer = MUST(demuxer_for(read_fixture("Ogg/theora_vorbis_10fps.ogg"sv)));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    auto producer = MUST(Media::DecodedVideoProducer::try_create(loop, demuxer, *track));
    producer->set_error_handler([](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("Decoding failed: {}", error.description()));
    });
    producer->start();

    Vector<AK::Duration> timestamps;
    EXPECT(spin_until([&] {
        while (true) {
            auto output = producer->peek();
            if (output.status == Media::PipelineStatus::HaveData) {
                timestamps.append(output.frame->timestamp());
                producer->consume();
                continue;
            }
            return output.status == Media::PipelineStatus::EndOfStream;
        }
    }));
    EXPECT_EQ(timestamps.size(), THEORA_FRAME_COUNT);
    for (size_t i = 0; i < timestamps.size(); i++)
        EXPECT_EQ(timestamps[i], AK::Duration::from_milliseconds(static_cast<i64>(i) * 100));
}

TEST_CASE(demuxer_seeks_video_to_the_keyframe_the_target_depends_on)
{
    auto demuxer = MUST(demuxer_for(read_fixture("Ogg/theora_vorbis_10fps.ogg"sv)));
    auto track = track_of_type(*demuxer, Media::TrackType::Video);

    for (auto target_milliseconds : { 1550, 1000, 2950, 50 }) {
        MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(target_milliseconds), Media::DemuxerSeekOptions::None));
        auto frame = MUST(demuxer->get_next_sample_for_track(track));
        EXPECT(frame.is_keyframe());
        EXPECT_EQ(frame.presentation_timestamp(), AK::Duration::from_milliseconds(static_cast<i64>(target_milliseconds / 1000) * 1000));
    }
}

// A seek lands before its target, early enough for Opus to have converged by then, and no more than a page earlier.
TEST_CASE(demuxer_seeks_audio_before_the_target)
{
    auto demuxer = MUST(demuxer_for(read_fixture("buffered-ranges/tone_opus.ogg"sv)));
    auto track = track_of_type(*demuxer, Media::TrackType::Audio);

    for (auto target_milliseconds : { 20'000, 3'500, 29'900, 100 }) {
        auto target = AK::Duration::from_milliseconds(target_milliseconds);
        MUST(demuxer->seek_to_most_recent_keyframe(track, target, Media::DemuxerSeekOptions::None));
        auto frame = MUST(demuxer->get_next_sample_for_track(track));
        auto start = frame.presentation_timestamp() - frame.leading_discard();
        EXPECT(start <= target - AK::Duration::from_milliseconds(80) || start <= AK::Duration::zero());
        EXPECT(start >= target - AK::Duration::from_milliseconds(1'100));
    }
}

TEST_CASE(demuxer_plays_through_a_chained_file)
{
    auto demuxer = MUST(demuxer_for(read_fixture("Ogg/chained_opus_then_vorbis.ogg"sv)));
    // A chained file's duration is learned by reading its links.
    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::zero());

    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks.size(), 1u);
    auto track = track_of_type(*demuxer, Media::TrackType::Audio);

    auto frames = read_every_frame(*demuxer, track);
    Optional<size_t> second_link_start;
    for (size_t i = 0; i < frames.size(); i++) {
        if (frames[i].codec_id() == Media::CodecID::Vorbis) {
            second_link_start = i;
            break;
        }
    }
    if (!second_link_start.has_value()) {
        FAIL("The second link's frames were not delivered");
        return;
    }
    auto const& first_vorbis_frame = frames[*second_link_start];
    EXPECT(first_vorbis_frame.new_codec_configuration().has_value());
    // The second link starts where the first ends.
    auto first_link_end = time_of_sample(OPUS_SAMPLE_COUNT);
    EXPECT(first_vorbis_frame.presentation_timestamp() >= first_link_end);
    EXPECT(first_vorbis_frame.presentation_timestamp() <= first_link_end + AK::Duration::from_milliseconds(50));
    for (size_t i = 1; i < frames.size(); i++)
        EXPECT(frames[i].presentation_timestamp() >= frames[i - 1].presentation_timestamp());
    auto end = frames.last().presentation_timestamp() + frames.last().duration();
    EXPECT_EQ(end, first_link_end + AK::Duration::from_seconds(1));
}

TEST_CASE(demuxer_grows_buffered_ranges_and_seeks_across_a_chain)
{
    auto demuxer = MUST(demuxer_for(read_fixture("Ogg/chained_opus_then_vorbis.ogg"sv)));
    auto track = track_of_type(*demuxer, Media::TrackType::Audio);

    demuxer->set_scan_state_change_handler([](auto const&) { });
    auto expected_end = time_of_sample(OPUS_SAMPLE_COUNT) + AK::Duration::from_seconds(1);
    EXPECT(spin_until([&] { return demuxer->scan_state().duration == expected_end; }));
    auto ranges = demuxer->scan_state().buffered_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    if (ranges.size() == 1)
        EXPECT_EQ(ranges[0].end, expected_end);

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(1'500), Media::DemuxerSeekOptions::None));
    auto frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(frame.codec_id(), Media::CodecID::Vorbis);
    EXPECT(frame.new_codec_configuration().has_value());
    EXPECT(frame.presentation_timestamp() <= AK::Duration::from_milliseconds(1'500));

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(500), Media::DemuxerSeekOptions::None));
    frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(frame.codec_id(), Media::CodecID::Opus);
    EXPECT(frame.new_codec_configuration().has_value());
    EXPECT(frame.presentation_timestamp() <= AK::Duration::from_milliseconds(500));
}

// An aborted read is retried by the next call, which delivers the frame the aborted read was after, so a pass
// interrupted anywhere delivers the same frames as one that was not.
TEST_CASE(demuxer_resumes_an_aborted_read_with_the_frame_it_was_reading)
{
    auto file_data = read_fixture("buffered-ranges/tone_vorbis.ogg"sv);

    Vector<FrameIdentity> expected_frames;
    {
        auto demuxer = MUST(demuxer_for(file_data));
        auto track = track_of_type(*demuxer, Media::TrackType::Audio);
        expected_frames = identities_of(read_every_frame(*demuxer, track));
    }

    // The data ends inside a page, so the read that blocks has part of its page. The tail is there for the duration.
    auto initial_chunk_size = (file_data.size() / 3) + 1000;
    auto tail_start = file_data.size() - (2 * Media::Ogg::MAXIMUM_PAGE_SIZE);
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(file_data.size());
    stream->add_chunk_at(0, file_data.bytes().trim(initial_chunk_size));
    stream->add_chunk_at(tail_start, file_data.bytes().slice(tail_start));

    IGNORE_USE_IN_ESCAPING_LAMBDA auto demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto track = track_of_type(*demuxer, Media::TrackType::Audio);

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
        actual_frames.extend(identities_of(read_every_frame(*demuxer, track)));
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

// A page that fails its CRC is skipped, losing only the packets it completes, and the packets after it keep their
// times since every page carries its own granule position.
TEST_CASE(demuxer_reads_on_past_a_damaged_page)
{
    auto file_data = read_fixture("buffered-ranges/tone_opus.ogg"sv);
    Vector<FrameIdentity> intact_frames;
    {
        auto demuxer = MUST(demuxer_for(file_data));
        auto track = track_of_type(*demuxer, Media::TrackType::Audio);
        intact_frames = identities_of(read_every_frame(*demuxer, track));
    }

    auto damaged_data = MUST(ByteBuffer::copy(file_data));
    damaged_data[damaged_data.size() / 2] ^= 0xFF;
    auto demuxer = MUST(demuxer_for(damaged_data));
    auto track = track_of_type(*demuxer, Media::TrackType::Audio);
    auto damaged_frames = identities_of(read_every_frame(*demuxer, track));

    EXPECT(damaged_frames.size() < intact_frames.size());
    EXPECT(damaged_frames.size() > intact_frames.size() - 100);
    EXPECT_EQ(damaged_frames.last(), intact_frames.last());
}

TEST_CASE(demuxer_declines_what_it_cannot_read)
{
    auto not_ogg = demuxer_for(read_fixture("WAV/tone_8000_mono.wav"sv));
    EXPECT(not_ogg.is_error());
    if (not_ogg.is_error())
        EXPECT_EQ(not_ogg.error().category(), Media::DecoderErrorCategory::UnrecognizedFormat);
}

// A seek into data that is not there yet asks for little more than the pages its probes land on.
TEST_CASE(demuxer_seeks_into_unbuffered_data_with_few_requests)
{
    auto file_data = read_fixture("buffered-ranges/tone_opus.ogg"sv);
    auto& loop = never_destroyed_event_loop();

    constexpr size_t head_size = 4 * KiB;
    IGNORE_USE_IN_ESCAPING_LAMBDA auto stream = Media::IncrementallyPopulatedStream::create_empty();
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
        auto slice_size = min<size_t>(8 * KiB, file_data.size() - *serving_position);
        stream->add_chunk_at(*serving_position, file_data.bytes().slice(*serving_position, slice_size));
        *serving_position += slice_size;
    });
    server->start();

    // The fetch that delivered the head was stopped, so each read of data that is not there asks for a request from
    // its own position, starting with the duration's read of the end of the file.
    stream->set_may_idle(true);
    stream->set_may_idle(false);

    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Media::Demuxer> demuxer;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> loaded { false };
    auto loader_thread = Threading::Thread::construct("TestLoader"sv, [&]() -> intptr_t {
        demuxer = MUST(Media::Ogg::OggDemuxer::from_stream(stream));
        loaded = true;
        return 0;
    });
    loader_thread->start();
    EXPECT(spin_until([&] { return loaded.load(); }));
    MUST(loader_thread->join());
    IGNORE_USE_IN_ESCAPING_LAMBDA auto track = track_of_type(*demuxer, Media::TrackType::Audio);
    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_seconds(30));

    // The player stopped the fetch once its buffer was full and lets it go again for the seek.
    stream->set_may_idle(true);
    stream->set_may_idle(false);
    serving_position = {};
    auto requests_before_seeking = requested_offsets.size();

    IGNORE_USE_IN_ESCAPING_LAMBDA auto target = AK::Duration::from_seconds(20);
    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Duration landed_timestamp;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> seek_finished { false };
    auto seeker_thread = Threading::Thread::construct("TestSeeker"sv, [&]() -> intptr_t {
        MUST(demuxer->seek_to_most_recent_keyframe(track, target, Media::DemuxerSeekOptions::None));
        landed_timestamp = MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp();
        seek_finished = true;
        return 0;
    });
    seeker_thread->start();

    if (!spin_until([&] { return seek_finished.load(); }))
        FAIL(ByteString::formatted("The seek did not finish: {} requests, serving at {}", requested_offsets.size(), serving_position));
    server->stop();
    stream->set_data_request_callback(nullptr);
    MUST(seeker_thread->join());
    (void)loop;

    EXPECT(landed_timestamp <= target);
    EXPECT(landed_timestamp >= target - AK::Duration::from_seconds(2));
    EXPECT(requested_offsets.size() - requests_before_seeking <= 3);
}

}
