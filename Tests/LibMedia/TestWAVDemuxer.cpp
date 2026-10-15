/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <LibCore/File.h>
#include <LibMedia/Containers/WAV/WAVDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

namespace {

struct WaveLayout {
    u16 format_tag { 1 };
    u16 channel_count { 2 };
    u32 sample_rate { 48000 };
    u16 bits_per_sample { 16 };
    // Writes the extensible form of the header.
    Optional<u32> speaker_position_mask {};
    u32 sample_data_size { 48000 * 4 };
    Optional<u32> declared_sample_data_size {};
    // An odd size checks that chunks are padded to even sizes.
    u32 list_chunk_size { 0 };
};

ByteBuffer make_wave(WaveLayout const& layout)
{
    static constexpr Array<u8, 14> media_subtype_guid_tail { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
    auto padded = [](u32 size) { return size + (size & 1); };
    auto bytes_per_frame = static_cast<u16>(layout.channel_count * ((layout.bits_per_sample + 7) / 8));
    u32 format_chunk_size = layout.speaker_position_mask.has_value() ? 40 : 16;
    u32 list_chunk_total_size = layout.list_chunk_size > 0 ? 8 + padded(layout.list_chunk_size) : 0;

    AllocatingMemoryStream wave;
    auto write_chunk_header = [&](StringView type, u32 size) {
        MUST(wave.write_until_depleted(type.bytes()));
        MUST(wave.write_value<LittleEndian<u32>>(size));
    };
    write_chunk_header("RIFF"sv, 4 + 8 + format_chunk_size + list_chunk_total_size + 8 + layout.sample_data_size);
    MUST(wave.write_until_depleted("WAVE"sv.bytes()));

    write_chunk_header("fmt "sv, format_chunk_size);
    MUST(wave.write_value<LittleEndian<u16>>(layout.speaker_position_mask.has_value() ? 0xFFFE : layout.format_tag));
    MUST(wave.write_value<LittleEndian<u16>>(layout.channel_count));
    MUST(wave.write_value<LittleEndian<u32>>(layout.sample_rate));
    MUST(wave.write_value<LittleEndian<u32>>(layout.sample_rate * bytes_per_frame));
    MUST(wave.write_value<LittleEndian<u16>>(bytes_per_frame));
    MUST(wave.write_value<LittleEndian<u16>>(layout.bits_per_sample));
    if (layout.speaker_position_mask.has_value()) {
        MUST(wave.write_value<LittleEndian<u16>>(22));
        MUST(wave.write_value<LittleEndian<u16>>(layout.bits_per_sample));
        MUST(wave.write_value<LittleEndian<u32>>(*layout.speaker_position_mask));
        MUST(wave.write_value<LittleEndian<u16>>(layout.format_tag));
        MUST(wave.write_until_depleted(media_subtype_guid_tail.span()));
    }

    auto silence = MUST(ByteBuffer::create_zeroed(max(layout.sample_data_size, list_chunk_total_size)));
    if (layout.list_chunk_size > 0) {
        write_chunk_header("LIST"sv, layout.list_chunk_size);
        MUST(wave.write_until_depleted(silence.bytes().trim(padded(layout.list_chunk_size))));
    }

    write_chunk_header("data"sv, layout.declared_sample_data_size.value_or(layout.sample_data_size));
    MUST(wave.write_until_depleted(silence.bytes().trim(layout.sample_data_size)));
    return MUST(wave.read_until_eof());
}

ByteBuffer read_fixture(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(file->read_until_eof());
}

Media::DecoderErrorOr<NonnullRefPtr<Media::Demuxer>> demuxer_for(ByteBuffer const& data)
{
    return Media::WAV::WAVDemuxer::from_stream(Media::IncrementallyPopulatedStream::create_from_buffer(data));
}

Media::Track audio_track(Media::Demuxer& demuxer)
{
    auto track = MUST(demuxer.get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    MUST(demuxer.create_context_for_track(*track));
    return *track;
}

struct PacketIdentity {
    AK::Duration timestamp;
    size_t byte_count { 0 };
    bool operator==(PacketIdentity const&) const = default;
};

Vector<PacketIdentity> read_every_packet(Media::Demuxer& demuxer, Media::Track const& track)
{
    Vector<PacketIdentity> packets;
    while (true) {
        auto packet = demuxer.get_next_sample_for_track(track);
        if (packet.is_error()) {
            EXPECT_EQ(packet.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        packets.append({ packet.value().presentation_timestamp(), packet.value().data().size() });
    }
    return packets;
}

TEST_CASE(demuxer_describes_the_only_track_of_the_stream)
{
    auto demuxer = MUST(demuxer_for(read_fixture("buffered-ranges/tone.wav"sv)));

    EXPECT(MUST(demuxer->get_tracks_for_type(Media::TrackType::Video)).is_empty());

    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.sample_rate(), 48000u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.channel_map(), Audio::ChannelMap::stereo());
    EXPECT_EQ(tracks[0].parsed_codec().codec_id(), Media::CodecID::S16LE);

    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_seconds(8));
}

TEST_CASE(demuxer_reads_every_sample_frame_with_exact_timestamps)
{
    auto demuxer = MUST(demuxer_for(read_fixture("buffered-ranges/tone.wav"sv)));
    auto track = audio_track(*demuxer);

    size_t packet_count = 0;
    i64 frame_count = 0;
    while (true) {
        auto packet = demuxer->get_next_sample_for_track(track);
        if (packet.is_error()) {
            EXPECT_EQ(packet.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        EXPECT_EQ(packet.value().presentation_timestamp(), AK::Duration::from_time_units(frame_count, 1, 48000));
        EXPECT_EQ(packet.value().data().size(), 4096u);
        EXPECT(packet.value().is_keyframe());
        EXPECT_EQ(packet.value().new_codec_configuration().has_value(), packet_count == 0);
        frame_count += static_cast<i64>(packet.value().data().size() / 4);
        packet_count++;
    }

    EXPECT_EQ(packet_count, 375u);
    EXPECT_EQ(frame_count, 384000);
}

TEST_CASE(demuxer_seeks_to_the_sample_frame_holding_a_timestamp)
{
    auto demuxer = MUST(demuxer_for(read_fixture("buffered-ranges/tone.wav"sv)));
    auto track = audio_track(*demuxer);

    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(3'500))), Media::DemuxerSeekResult::MovedPosition);
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_milliseconds(3'500));

    // A target between two sample frames lands on the earlier one.
    auto target = AK::Duration::from_microseconds(1'000'010);
    MUST(demuxer->seek_to_most_recent_keyframe(track, target));
    auto packet = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(packet.presentation_timestamp(), AK::Duration::from_seconds(1));
    EXPECT(packet.presentation_timestamp() + AK::Duration::from_time_units(1, 1, 48000) > target);

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(10)));
    EXPECT_EQ(demuxer->get_next_sample_for_track(track).error().category(), Media::DecoderErrorCategory::EndOfStream);

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::zero(), Media::DemuxerSeekOptions::NeedCodecConfiguration));
    packet = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(packet.presentation_timestamp(), AK::Duration::zero());
    EXPECT(packet.new_codec_configuration().has_value());
}

// An aborted read is retried by the next call, which delivers the packet the aborted read was after, so a pass
// interrupted anywhere delivers the same packets as one that was not.
TEST_CASE(demuxer_resumes_an_aborted_read_with_the_packet_it_was_reading)
{
    auto file_data = read_fixture("buffered-ranges/tone.wav"sv);

    Vector<PacketIdentity> expected_packets;
    {
        auto demuxer = MUST(demuxer_for(file_data));
        auto track = audio_track(*demuxer);
        expected_packets = read_every_packet(*demuxer, track);
    }

    // The data ends inside a packet, so the read that blocks has part of its packet.
    auto initial_chunk_size = (file_data.size() / 3) + 1000;
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(file_data.size());
    stream->add_chunk_at(0, file_data.bytes().trim(initial_chunk_size));

    IGNORE_USE_IN_ESCAPING_LAMBDA auto demuxer = MUST(Media::WAV::WAVDemuxer::from_stream(stream));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto track = audio_track(*demuxer);

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_was_aborted { false };
    demuxer->set_read_blocked_change_handler_for_track(track, [&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<PacketIdentity> actual_packets;
    auto reader_thread = Threading::Thread::construct("TestReader"sv, [&]() -> intptr_t {
        while (true) {
            auto packet = demuxer->get_next_sample_for_track(track);
            if (packet.is_error()) {
                EXPECT_EQ(packet.error().category(), Media::DecoderErrorCategory::Aborted);
                break;
            }
            actual_packets.append({ packet.value().presentation_timestamp(), packet.value().data().size() });
        }
        // Like a producer, the reading thread allows reads again before it reads on.
        demuxer->reset_blocking_reads_aborted_for_track(track);
        read_was_aborted = true;
        actual_packets.extend(read_every_packet(*demuxer, track));
        return 0;
    });
    reader_thread->start();

    while (!read_blocked.load()) { }
    demuxer->set_blocking_reads_aborted_for_track(track);
    while (!read_was_aborted.load()) { }

    stream->add_chunk_at(initial_chunk_size, file_data.bytes().slice(initial_chunk_size));
    stream->close();
    MUST(reader_thread->join());

    EXPECT_EQ(actual_packets.size(), expected_packets.size());
    for (size_t index = 0; index < min(actual_packets.size(), expected_packets.size()); index++) {
        if (actual_packets[index] != expected_packets[index]) {
            FAIL(ByteString::formatted("Packet {} differs after the abort: {}ms/{} bytes instead of {}ms/{} bytes", index, actual_packets[index].timestamp.to_milliseconds(), actual_packets[index].byte_count, expected_packets[index].timestamp.to_milliseconds(), expected_packets[index].byte_count));
            break;
        }
    }
}

Audio::ChannelMap channel_map_of(WaveLayout const& layout)
{
    auto demuxer = MUST(demuxer_for(make_wave(layout)));
    return MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio))[0].audio_data().sample_specification.channel_map();
}

TEST_CASE(demuxer_reads_speaker_positions_from_an_extensible_header)
{
    using Audio::Channel;
    constexpr u32 front_left = 1 << 0;
    constexpr u32 front_right = 1 << 1;
    constexpr u32 side_left = 1 << 9;
    constexpr u32 side_right = 1 << 10;

    EXPECT_EQ(channel_map_of({ .channel_count = 4, .speaker_position_mask = front_left | front_right | side_left | side_right }), Audio::ChannelMap(Channel::FrontLeft, Channel::FrontRight, Channel::SideLeft, Channel::SideRight));
    EXPECT_EQ(channel_map_of({ .channel_count = 6, .speaker_position_mask = 0x3F }), Audio::ChannelMap::surround_5_1());

    // Positions that do not account for every channel are ignored in favour of the usual layout for the count.
    EXPECT_EQ(channel_map_of({ .channel_count = 6, .speaker_position_mask = front_left | front_right }), Audio::ChannelMap::surround_5_1());
    EXPECT_EQ(channel_map_of({ .channel_count = 6 }), Audio::ChannelMap::surround_5_1());
    EXPECT_EQ(channel_map_of({ .channel_count = 1 }), Audio::ChannelMap::mono());

    auto unplaced = channel_map_of({ .channel_count = 3 });
    EXPECT_EQ(unplaced.channel_count(), 3);
    EXPECT_EQ(unplaced.channel_at(0), Channel::Unknown);
}

Media::CodecID codec_id_of(WaveLayout const& layout)
{
    auto demuxer = MUST(demuxer_for(make_wave(layout)));
    return MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio))[0].parsed_codec().codec_id();
}

TEST_CASE(demuxer_identifies_every_sample_format_it_supports)
{
    EXPECT_EQ(codec_id_of({ .bits_per_sample = 8 }), Media::CodecID::U8);
    EXPECT_EQ(codec_id_of({ .bits_per_sample = 16 }), Media::CodecID::S16LE);
    EXPECT_EQ(codec_id_of({ .bits_per_sample = 24 }), Media::CodecID::S24LE);
    EXPECT_EQ(codec_id_of({ .bits_per_sample = 32 }), Media::CodecID::S32LE);
    EXPECT_EQ(codec_id_of({ .format_tag = 3, .bits_per_sample = 32 }), Media::CodecID::F32LE);
    EXPECT_EQ(codec_id_of({ .format_tag = 6, .bits_per_sample = 8 }), Media::CodecID::ALaw);
    EXPECT_EQ(codec_id_of({ .format_tag = 7, .bits_per_sample = 8 }), Media::CodecID::MuLaw);
    EXPECT_EQ(codec_id_of({ .format_tag = 3, .bits_per_sample = 32, .speaker_position_mask = 0x3 }), Media::CodecID::F32LE);
}

TEST_CASE(demuxer_declines_what_it_cannot_read)
{
    auto not_a_wave = demuxer_for(MUST(ByteBuffer::create_zeroed(4096)));
    EXPECT_EQ(not_a_wave.error().category(), Media::DecoderErrorCategory::UnrecognizedFormat);

    auto adpcm = demuxer_for(make_wave({ .format_tag = 0x11, .bits_per_sample = 4 }));
    EXPECT_EQ(adpcm.error().category(), Media::DecoderErrorCategory::NotImplemented);

    auto double_precision = demuxer_for(make_wave({ .format_tag = 3, .bits_per_sample = 64 }));
    EXPECT_EQ(double_precision.error().category(), Media::DecoderErrorCategory::NotImplemented);

    auto header_without_data = make_wave({});
    header_without_data.resize(12 + 8 + 16);
    auto no_data_chunk = demuxer_for(header_without_data);
    EXPECT_EQ(no_data_chunk.error().category(), Media::DecoderErrorCategory::Corrupted);
}

TEST_CASE(demuxer_reads_to_the_end_of_a_stream_whose_data_size_is_left_open)
{
    constexpr u32 frame_count = 2500;
    auto demuxer = MUST(demuxer_for(make_wave({ .sample_data_size = frame_count * 4, .declared_sample_data_size = NumericLimits<u32>::max(), .list_chunk_size = 7 })));
    auto track = audio_track(*demuxer);

    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_time_units(frame_count, 1, 48000));

    auto packets = read_every_packet(*demuxer, track);
    size_t byte_count = 0;
    for (auto const& packet : packets)
        byte_count += packet.byte_count;
    EXPECT_EQ(byte_count, frame_count * 4u);
    EXPECT_EQ(packets.last().timestamp, AK::Duration::from_time_units(frame_count - (packets.last().byte_count / 4), 1, 48000));
}

}
