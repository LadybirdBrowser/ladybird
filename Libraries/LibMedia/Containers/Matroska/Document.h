/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/FixedArray.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

namespace Media::Matroska {

struct EBMLHeader {
    String doc_type;
    u32 doc_type_version;
};

class SegmentInformation {
public:
    u64 timestamp_scale() const { return m_timestamp_scale; }
    void set_timestamp_scale(u64 timestamp_scale) { m_timestamp_scale = timestamp_scale; }
    StringView muxing_app() const LIFETIME_BOUND { return m_muxing_app; }
    void set_muxing_app(String muxing_app) { m_muxing_app = move(muxing_app); }
    StringView writing_app() const LIFETIME_BOUND { return m_writing_app; }
    void set_writing_app(String writing_app) { m_writing_app = move(writing_app); }
    Optional<double> duration_unscaled() const { return m_duration_unscaled; }
    void set_duration_unscaled(double duration) { m_duration_unscaled.emplace(duration); }
    Optional<AK::Duration> duration() const
    {
        if (!duration_unscaled().has_value())
            return {};
        return AK::Duration::from_nanoseconds(static_cast<i64>(static_cast<double>(timestamp_scale()) * duration_unscaled().value()));
    }

private:
    u64 m_timestamp_scale { 1'000'000 };
    String m_muxing_app;
    String m_writing_app;
    Optional<double> m_duration_unscaled;
};

class TrackEntry : public RefCounted<TrackEntry> {
public:
    enum TrackType : u8 {
        Invalid = 0,
        Video = 1,
        Audio = 2,
        Complex = 3,
        Logo = 16,
        Subtitle = 17,
        Buttons = 18,
        Control = 32,
        Metadata = 33,
    };

    enum class ColorRange : u8 {
        Unspecified = 0,
        Broadcast = 1,
        Full = 2,
        UseCICP = 3, // defined by MatrixCoefficients / TransferCharacteristics
    };

    struct ColorFormat {
        ColorPrimaries color_primaries = ColorPrimaries::Unspecified;
        TransferCharacteristics transfer_characteristics = TransferCharacteristics::Unspecified;
        MatrixCoefficients matrix_coefficients = MatrixCoefficients::Unspecified;
        u64 bits_per_channel = 0;
        ColorRange range = ColorRange::Unspecified;

        CodingIndependentCodePoints to_cicp() const
        {
            VideoFullRangeFlag video_full_range_flag;
            switch (range) {
            case ColorRange::Full:
                video_full_range_flag = VideoFullRangeFlag::Full;
                break;
            case ColorRange::Broadcast:
                video_full_range_flag = VideoFullRangeFlag::Studio;
                break;
            case ColorRange::Unspecified:
            case ColorRange::UseCICP:
                // FIXME: Figure out what UseCICP should do here. Matroska specification did not
                //        seem to explain in the 'colour' section. When this is fixed, change
                //        replace_code_points_if_specified to match.
                video_full_range_flag = VideoFullRangeFlag::Unspecified;
                break;
            }

            return { color_primaries, transfer_characteristics, matrix_coefficients, video_full_range_flag };
        }
    };

    struct VideoTrack {
        u64 pixel_width { 0 };
        u64 pixel_height { 0 };

        ColorFormat color_format;
    };

    struct AudioTrack {
        u64 channels { 1 };
        double sampling_frequency { 8000.0 };
        u64 bit_depth { 0 };
    };

    u64 track_number() const { return m_track_number; }
    void set_track_number(u64 track_number) { m_track_number = track_number; }
    u64 track_uid() const { return m_track_uid; }
    void set_track_uid(u64 track_uid) { m_track_uid = track_uid; }
    TrackType track_type() const { return m_track_type; }
    void set_track_type(TrackType track_type) { m_track_type = track_type; }
    String name() const { return m_name; }
    void set_name(String const& name) { m_name = name; }
    String language() const { return m_language; }
    void set_language(String const& language) { m_language = language; }
    Optional<String> const& language_bcp_47() const { return m_language_bcp_47; }
    void set_language_bcp_47(String const& language_bcp_47) { m_language_bcp_47 = language_bcp_47; }
    String codec_id() const { return m_codec_id; }
    void set_codec_id(String const& codec_id) { m_codec_id = codec_id; }
    ReadonlyBytes codec_private_data() const LIFETIME_BOUND { return m_codec_private_data.span(); }
    ErrorOr<void> set_codec_private_data(ReadonlyBytes codec_private_data)
    {
        m_codec_private_data = TRY(FixedArray<u8>::create(codec_private_data));
        return {};
    }
    double timestamp_scale() const { return m_timestamp_scale; }
    void set_timestamp_scale(double timestamp_scale) { m_timestamp_scale = timestamp_scale; }
    u64 codec_delay() const { return m_codec_delay; }
    void set_codec_delay(u64 codec_delay) { m_codec_delay = codec_delay; }
    u64 seek_pre_roll() const { return m_seek_pre_roll; }
    void set_seek_pre_roll(u64 seek_pre_roll) { m_seek_pre_roll = seek_pre_roll; }
    u64 timestamp_offset() const { return m_timestamp_offset; }
    void set_timestamp_offset(u64 timestamp_offset) { m_timestamp_offset = timestamp_offset; }
    Optional<VideoTrack> video_track() const { return m_video_track; }
    void set_video_track(VideoTrack video_track) { m_video_track = video_track; }
    Optional<AudioTrack> audio_track() const { return m_audio_track; }
    void set_audio_track(AudioTrack audio_track) { m_audio_track = audio_track; }

private:
    u64 m_track_number { 0 };
    u64 m_track_uid { 0 };
    TrackType m_track_type { Invalid };
    String m_name;
    String m_language = "eng"_string;
    Optional<String> m_language_bcp_47;
    String m_codec_id;
    FixedArray<u8> m_codec_private_data;
    double m_timestamp_scale { 1 };
    u64 m_codec_delay { 0 };
    u64 m_seek_pre_roll { 0 };
    u64 m_timestamp_offset { 0 };
    Optional<VideoTrack> m_video_track;
    Optional<AudioTrack> m_audio_track;
};

class Block {
public:
    enum Lacing : u8 {
        None = 0b00,
        XIPH = 0b01,
        FixedSize = 0b10,
        EBML = 0b11,
    };

    u64 track_number() const { return m_track_number; }
    void set_track_number(u64 track_number) { m_track_number = track_number; }
    AK::Duration timestamp() const { return m_timestamp; }
    void set_timestamp(AK::Duration timestamp) { m_timestamp = timestamp; }
    bool only_keyframes() const { return m_only_keyframes; }
    void set_only_keyframes(bool only_keyframes) { m_only_keyframes = only_keyframes; }
    bool invisible() const { return m_invisible; }
    void set_invisible(bool invisible) { m_invisible = invisible; }
    Lacing lacing() const { return m_lacing; }
    void set_lacing(Lacing lacing) { m_lacing = lacing; }
    bool discardable() const { return m_discardable; }
    void set_discardable(bool discardable) { m_discardable = discardable; }

    void set_frames(Vector<ReadonlyBytes>&& frames) { m_frames = move(frames); }
    ReadonlyBytes const& frame(size_t index) const { return frames()[index]; }
    u64 frame_count() const { return m_frames.size(); }
    Vector<ReadonlyBytes> const& frames() const { return m_frames; }

private:
    u64 m_track_number { 0 };
    AK::Duration m_timestamp { AK::Duration::zero() };
    bool m_only_keyframes { false };
    bool m_invisible { false };
    Lacing m_lacing { None };
    bool m_discardable { true };
    Vector<ReadonlyBytes> m_frames;
};

class Cluster {
public:
    AK::Duration timestamp() const { return m_timestamp; }
    void set_timestamp(AK::Duration timestamp) { m_timestamp = timestamp; }

private:
    AK::Duration m_timestamp { AK::Duration::zero() };
};

class CueTrackPosition {
public:
    u64 track_number() const { return m_track_number; }
    void set_track_number(u64 track_number) { m_track_number = track_number; }
    size_t cluster_position() const { return m_cluster_position; }
    void set_cluster_position(size_t cluster_position) { m_cluster_position = cluster_position; }
    size_t block_offset() const { return m_block_offset; }
    void set_block_offset(size_t block_offset) { m_block_offset = block_offset; }

private:
    u64 m_track_number { 0 };
    size_t m_cluster_position { 0 };
    size_t m_block_offset { 0 };
};

class CuePoint {
public:
    AK::Duration timestamp() const { return m_timestamp; }
    void set_timestamp(AK::Duration timestamp) { m_timestamp = timestamp; }
    OrderedHashMap<u64, CueTrackPosition>& track_positions() { return m_track_positions; }
    OrderedHashMap<u64, CueTrackPosition> const& track_positions() const { return m_track_positions; }
    Optional<CueTrackPosition const&> position_for_track(u64 track_number) const { return m_track_positions.get(track_number); }

private:
    AK::Duration m_timestamp = AK::Duration::min();
    OrderedHashMap<u64, CueTrackPosition> m_track_positions;
};

}
