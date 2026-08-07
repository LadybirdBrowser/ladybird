/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/Containers/Matroska/Utilities.h>
#include <LibTest/TestCase.h>

static Media::CodecID codec_id_for_pcm_track(StringView matroska_codec_id, u64 bit_depth)
{
    auto track_entry = make_ref_counted<Media::Matroska::TrackEntry>();
    track_entry->set_codec_id(MUST(String::from_utf8(matroska_codec_id)));
    track_entry->set_audio_track({ .bit_depth = bit_depth });
    return Media::Matroska::codec_id_from_matroska_track_entry(*track_entry);
}

TEST_CASE(pcm_codec_ids)
{
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/FLOAT/IEEE"sv, 32), Media::CodecID::F32LE);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/FLOAT/IEEE"sv, 64), Media::CodecID::Unknown);

    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/BIG"sv, 8), Media::CodecID::U8);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/BIG"sv, 16), Media::CodecID::Unknown);

    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/LIT"sv, 8), Media::CodecID::U8);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/LIT"sv, 16), Media::CodecID::S16LE);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/LIT"sv, 24), Media::CodecID::S24LE);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/LIT"sv, 32), Media::CodecID::S32LE);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/LIT"sv, 20), Media::CodecID::Unknown);

    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/FLOAT/IEEE"sv, 0), Media::CodecID::Unknown);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/BIG"sv, 0), Media::CodecID::Unknown);
    EXPECT_EQ(codec_id_for_pcm_track("A_PCM/INT/LIT"sv, 0), Media::CodecID::Unknown);
}

TEST_CASE(seek_past_eos)
{
    auto file = MUST(Core::File::open("./vfr.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream));

    auto optional_track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    EXPECT(optional_track.has_value());
    auto track = optional_track.release_value();
    MUST(demuxer->create_context_for_track(track));

    AK::Duration last_timestamp;
    while (true) {
        auto sample_result = demuxer->get_next_sample_for_track(track);
        if (sample_result.is_error()) {
            EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        last_timestamp = sample_result.release_value().presentation_timestamp();
    }
    EXPECT_EQ(last_timestamp, AK::Duration::from_milliseconds(30126));

    auto seek_time = AK::Duration::from_milliseconds(31000);
    auto seek_result = MUST(demuxer->seek_to_most_recent_keyframe(track, seek_time, Media::DemuxerSeekOptions::None));
    EXPECT_EQ(seek_result, Media::DemuxerSeekResult::KeptCurrentPosition);
    auto sample_result_after_seek = demuxer->get_next_sample_for_track(track);
    EXPECT(sample_result_after_seek.is_error());
    EXPECT_EQ(sample_result_after_seek.error().category(), Media::DecoderErrorCategory::EndOfStream);
}
