/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/Codecs/AV1.h>
#include <LibMedia/Codecs/H264.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFramePool.h>
#include <LibMedia/VideoSurface.h>
#include <LibMedia/VideoToolbox/VideoToolboxVideoDecoder.h>

#include "TestMediaCommon.h"

namespace {

struct Decoding {
    NonnullOwnPtr<Media::VideoToolbox::VideoToolboxVideoDecoder> decoder;
    NonnullRefPtr<Media::Demuxer> demuxer;
    Media::Track track;

    Media::DecoderErrorOr<NonnullRefPtr<Media::VideoFrame>> next_frame() const
    {
        while (true) {
            auto frame_result = decoder->take_next_output(track.video_data().cicp);
            if (!frame_result.is_error() || frame_result.error().category() != Media::DecoderErrorCategory::NeedsMoreInput)
                return frame_result;

            auto sample_result = demuxer->get_next_sample_for_track(track);
            if (sample_result.is_error()) {
                decoder->signal_end_of_stream();
                continue;
            }
            TRY(decoder->receive_coded_data(sample_result.release_value(), Media::DecodeIntent::Output));
        }
    }
};

Decoding open_decoding(StringView path, Media::CodecID codec_id)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto decoder = MUST(Media::VideoToolbox::VideoToolboxVideoDecoder::try_create(codec_id, {}));
    return Decoding { move(decoder), move(demuxer), track.release_value() };
}

// A Mac without this codec in hardware has nothing to test.
bool hardware_decoding_is_unavailable(Media::DecoderError const& error)
{
    return error.category() == Media::DecoderErrorCategory::NotImplemented;
}

}

TEST_CASE(decodes_vp9_onto_surfaces)
{
    auto decoding = open_decoding("./vp9_in_webm.webm"sv, Media::CodecID::VP9);

    auto last_timestamp = AK::Duration::min();
    size_t frame_count = 0;

    while (true) {
        auto frame_result = decoding.next_frame();
        if (frame_result.is_error()) {
            if (hardware_decoding_is_unavailable(frame_result.error()))
                return;
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        auto frame = frame_result.release_value();

        EXPECT_NE(frame->surface(), nullptr);
        // The pixels live on the surface, so there are no planes in the slot's buffer to view.
        EXPECT(!frame->yuv_data().has_value());
        EXPECT_EQ(frame->size(), Gfx::Size<u32>(854, 480));
        EXPECT(last_timestamp <= frame->timestamp());
        last_timestamp = frame->timestamp();
        frame_count++;
    }

    EXPECT_EQ(frame_count, 25u);
}

TEST_CASE(a_format_change_mid_stream_rebuilds_the_session)
{
    // A stream that changes format has to tear the session down, or its later frames are decoded against a
    // description that no longer matches them.
    auto decoding = open_decoding("./vp9_resize.webm"sv, Media::CodecID::VP9);

    Vector<Gfx::Size<u32>> sizes;
    while (true) {
        auto frame_result = decoding.next_frame();
        if (frame_result.is_error()) {
            if (hardware_decoding_is_unavailable(frame_result.error()))
                return;
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        auto size = frame_result.release_value()->size();
        if (sizes.is_empty() || sizes.last() != size)
            sizes.append(size);
    }

    EXPECT_EQ(sizes.size(), 2u);
    if (sizes.size() == 2) {
        EXPECT_EQ(sizes[0], Gfx::Size<u32>(320, 240));
        EXPECT_EQ(sizes[1], Gfx::Size<u32>(640, 480));
    }
}

TEST_CASE(surfaces_are_not_reused_while_their_frames_are_held)
{
    auto decoding = open_decoding("./vp9_in_webm.webm"sv, Media::CodecID::VP9);

    // The media engine recycles surfaces as soon as they fall out of use, and holding a frame is what has to keep
    // one in use. Were that claim missing, these frames would be a handful of surfaces overwriting one another.
    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    HashTable<u32> held_surface_ids;
    while (held_frames.size() < Media::VideoFrameSurfacePool::MAX_SLOT_COUNT) {
        auto frame_result = decoding.next_frame();
        if (frame_result.is_error() && hardware_decoding_is_unavailable(frame_result.error()))
            return;
        auto frame = TRY_OR_FAIL(move(frame_result));
        auto surface = frame->surface();
        EXPECT_NE(surface, nullptr);
        EXPECT(!held_surface_ids.contains(surface->id()));
        held_surface_ids.set(surface->id());
        held_frames.append(move(frame));
    }
}

TEST_CASE(storage_runs_out_while_every_frame_is_held)
{
    auto decoding = open_decoding("./vp9_in_webm.webm"sv, Media::CodecID::VP9);

    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    while (held_frames.size() < Media::VideoFrameSurfacePool::MAX_SLOT_COUNT) {
        auto frame_result = decoding.next_frame();
        if (frame_result.is_error() && hardware_decoding_is_unavailable(frame_result.error()))
            return;
        held_frames.append(TRY_OR_FAIL(move(frame_result)));
    }

    auto exhausted_result = decoding.next_frame();
    EXPECT(exhausted_result.is_error());
    EXPECT_EQ(exhausted_result.error().category(), Media::DecoderErrorCategory::TryAgain);

    // Letting the first frame go returns its slot to the pool.
    held_frames.remove(0);
    EXPECT(!decoding.next_frame().is_error());
}

TEST_CASE(only_the_profiles_the_hardware_covers_are_claimed)
{
    auto vp9_codec = [](u8 profile, u8 bit_depth, Media::Subsampling subsampling) {
        return Media::ParsedCodec { Media::Codecs::VP9::Parameters {
            .profile = profile,
            .level = 10,
            .bit_depth = bit_depth,
            .color_parameters = { .subsampling = subsampling, .cicp = {} },
        } };
    };

    using Decoder = Media::VideoToolbox::VideoToolboxVideoDecoder;

    // Without a hardware decoder at all there is nothing to distinguish, so this says nothing either way.
    if (!Decoder::capabilities(Media::ParsedCodec { Media::CodecID::VP9 }).has_value()) {
        warnln("No hardware VP9 decoder available, skipping");
        return;
    }

    EXPECT(Decoder::capabilities(vp9_codec(0, 8, Media::Subsampling::yuv420())).has_value());
    EXPECT(Decoder::capabilities(vp9_codec(2, 10, Media::Subsampling::yuv420())).has_value());

    // 4:2:2 and 4:4:4 have no hardware behind them, and no Apple decoder reaches 12 bits.
    EXPECT(!Decoder::capabilities(vp9_codec(1, 8, Media::Subsampling::yuv422())).has_value());
    EXPECT(!Decoder::capabilities(vp9_codec(3, 10, Media::Subsampling::yuv444())).has_value());
    EXPECT(!Decoder::capabilities(vp9_codec(2, 12, Media::Subsampling::yuv420())).has_value());
}

TEST_CASE(what_the_hardware_decodes_is_claimed_power_efficient)
{
    auto capabilities = Media::VideoToolbox::VideoToolboxVideoDecoder::capabilities(Media::ParsedCodec { Media::CodecID::VP9 });
    if (!capabilities.has_value()) {
        warnln("No hardware VP9 decoder available, skipping");
        return;
    }

    // Hardware is the only thing this decoder is ever chosen for, so anything it claims it claims as power efficient.
    EXPECT(capabilities->power_efficient);
}

TEST_CASE(decodes_av1_onto_surfaces)
{
    auto decoding = open_decoding("./av1_in_webm.webm"sv, Media::CodecID::AV1);

    auto last_timestamp = AK::Duration::min();
    size_t frame_count = 0;

    while (true) {
        auto frame_result = decoding.next_frame();
        if (frame_result.is_error()) {
            if (hardware_decoding_is_unavailable(frame_result.error()))
                return;
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        auto frame = frame_result.release_value();

        EXPECT_NE(frame->surface(), nullptr);
        EXPECT_EQ(frame->size(), Gfx::Size<u32>(854, 480));
        EXPECT(last_timestamp <= frame->timestamp());
        last_timestamp = frame->timestamp();
        frame_count++;
    }

    EXPECT_EQ(frame_count, 25u);
}

TEST_CASE(only_the_av1_profiles_the_hardware_covers_are_claimed)
{
    auto av1_codec = [](u8 profile, u8 bit_depth, Media::Subsampling subsampling) {
        return Media::ParsedCodec { Media::Codecs::AV1::Parameters {
            .profile = profile,
            .level = 8,
            .tier = Media::Codecs::AV1::Tier::Main,
            .bit_depth = bit_depth,
            .optional_fields = { .monochrome = false, .subsampling = subsampling, .chroma_sample_position = 0, .cicp = {} },
        } };
    };

    using Decoder = Media::VideoToolbox::VideoToolboxVideoDecoder;

    if (!Decoder::capabilities(av1_codec(0, 8, Media::Subsampling::yuv420())).has_value()) {
        warnln("No hardware AV1 decoder available, skipping");
        return;
    }

    EXPECT(Decoder::capabilities(av1_codec(2, 10, Media::Subsampling::yuv420())).has_value());

    // Only the profile that subsamples both axes has hardware behind it.
    EXPECT(!Decoder::capabilities(av1_codec(1, 8, Media::Subsampling { false, false })).has_value());
}

// Taking output as it becomes available, the way playback does, is the only shape that can catch a frame handed
// over before an earlier one has arrived. This clip codes its frames out of display order.
TEST_CASE(decodes_h264_onto_surfaces_in_display_order)
{
    auto decoding = open_decoding("./avc.mp4"sv, Media::CodecID::H264);

    auto last_timestamp = AK::Duration::min();
    size_t frame_count = 0;

    while (true) {
        auto frame_result = decoding.next_frame();
        if (frame_result.is_error()) {
            if (hardware_decoding_is_unavailable(frame_result.error()))
                return;
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        auto frame = frame_result.release_value();

        EXPECT_NE(frame->surface(), nullptr);
        EXPECT_EQ(frame->size(), Gfx::Size<u32>(854, 480));
        EXPECT(last_timestamp <= frame->timestamp());
        last_timestamp = frame->timestamp();
        frame_count++;
    }

    EXPECT_EQ(frame_count, 50u);
}

TEST_CASE(decodes_h265_onto_surfaces_in_display_order)
{
    auto decoding = open_decoding("./hevc_10bit.mp4"sv, Media::CodecID::H265);

    auto last_timestamp = AK::Duration::min();
    size_t frame_count = 0;

    while (true) {
        auto frame_result = decoding.next_frame();
        if (frame_result.is_error()) {
            if (hardware_decoding_is_unavailable(frame_result.error()))
                return;
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        auto frame = frame_result.release_value();

        EXPECT_NE(frame->surface(), nullptr);
        EXPECT_EQ(frame->size(), Gfx::Size<u32>(560, 320));
        EXPECT_EQ(frame->bit_depth(), 10);
        EXPECT(last_timestamp <= frame->timestamp());
        last_timestamp = frame->timestamp();
        frame_count++;
    }

    EXPECT_EQ(frame_count, 30u);
}

TEST_CASE(h264_parameter_sets_can_arrive_separately_and_outlive_their_frames)
{
    for (bool sequence_in_band : { false, true }) {
        auto decoding = open_decoding("./avc.mp4"sv, Media::CodecID::H264);
        auto sample = MUST(decoding.demuxer->get_next_sample_for_track(decoding.track));
        auto configuration = sample.new_codec_configuration();
        VERIFY(configuration.has_value());
        auto sets = Media::Codecs::H264::parse_parameter_sets_from_configuration_record(*configuration);
        VERIFY(sets.has_value());
        VERIFY(sets->nal_unit_length_size == 4);

        auto const& initial_sequence = sets->sequence;
        auto const& initial_picture = sets->picture;
        ByteBuffer initial_configuration = MUST(ByteBuffer::copy(configuration->trim(5)));
        MUST(initial_configuration.try_append(static_cast<u8>(0xe0 | (sequence_in_band ? 0 : initial_sequence.size()))));
        auto append_sets = [](ByteBuffer& output, auto const& parameter_sets) {
            for (auto nal_unit : parameter_sets) {
                MUST(output.try_append(static_cast<u8>(nal_unit.size() >> 8)));
                MUST(output.try_append(static_cast<u8>(nal_unit.size())));
                MUST(output.try_append(nal_unit));
            }
        };
        if (!sequence_in_band)
            append_sets(initial_configuration, initial_sequence);
        MUST(initial_configuration.try_append(static_cast<u8>(sequence_in_band ? initial_picture.size() : 0)));
        if (sequence_in_band)
            append_sets(initial_configuration, initial_picture);
        decoding.decoder = MUST(Media::VideoToolbox::VideoToolboxVideoDecoder::try_create(Media::CodecID::H264, initial_configuration));

        {
            ByteBuffer data;
            auto const& in_band_sets = sequence_in_band ? initial_sequence : initial_picture;
            for (auto nal_unit : in_band_sets) {
                for (int shift = 24; shift >= 0; shift -= 8)
                    MUST(data.try_append(static_cast<u8>(nal_unit.size() >> shift)));
                MUST(data.try_append(nal_unit));
            }
            MUST(data.try_append(sample.data()));
            Media::CodedFrame frame { sample.codec_id(), sample.presentation_timestamp(), sample.decode_timestamp(), sample.duration(), sample.flags(), MUST(FixedArray<u8>::create(data.bytes())) };
            auto result = decoding.decoder->receive_coded_data(frame, Media::DecodeIntent::Output);
            if (result.is_error() && hardware_decoding_is_unavailable(result.error())) {
                warnln("No hardware H.264 decoder available, skipping");
                return;
            }
            EXPECT(!result.is_error());
            if (result.is_error())
                return;
        }

        // The in-band frame's storage is gone; subsequent frames have no parameter-set updates.
        size_t frame_count = 0;
        auto last_timestamp = AK::Duration::min();
        while (true) {
            auto result = decoding.next_frame();
            if (result.is_error()) {
                EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::EndOfStream);
                break;
            }
            auto frame = result.release_value();
            EXPECT(last_timestamp <= frame->timestamp());
            last_timestamp = frame->timestamp();
            ++frame_count;
        }
        EXPECT_EQ(frame_count, 50u);
    }
}
