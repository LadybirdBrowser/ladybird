/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Containers/ISOBMFF/BoxTypes.h>
#include <LibMedia/Track.h>

namespace Media::ISOBMFF {

struct BoxHeader {
    FourCC type;
    size_t position { 0 };
    size_t content_position { 0 };
    // Empty when the box's size field was zero, meaning that it extends to the end of the stream.
    Optional<size_t> content_size;

    Optional<size_t> end_position() const
    {
        if (!content_size.has_value())
            return {};
        return content_position + content_size.value();
    }
};

struct FullBoxHeader {
    u8 version { 0 };
    u32 flags { 0 };
};

struct FileType {
    FourCC major_brand;
    u32 minor_version { 0 };
    Vector<FourCC> compatible_brands;

    bool is_compatible_with(FourCC brand) const
    {
        return major_brand == brand || compatible_brands.contains_slow(brand);
    }
};

enum class HandlerType : u8 {
    Unknown,
    Video,
    Audio,
    Text,
};

struct VideoSampleData {
    u16 width { 0 };
    u16 height { 0 };
    Optional<CodingIndependentCodePoints> cicp;
};

struct AudioSampleData {
    u16 channel_count { 0 };
    u16 bits_per_sample { 0 };
    u32 sample_rate { 0 };
};

struct SampleEntry {
    FourCC format;
    CodecID codec_id { CodecID::Unknown };
    FixedArray<u8> codec_initialization_data;
    Optional<VideoSampleData> video;
    Optional<AudioSampleData> audio;
};

struct TrackFragmentSampleDefaults {
    u32 sample_description_index { 1 };
    u32 sample_duration { 0 };
    u32 sample_size { 0 };
    u32 sample_flags { 0 };
};

struct TrackEntry : public RefCounted<TrackEntry> {
    u32 track_id { 0 };
    bool is_enabled { true };
    HandlerType handler_type { HandlerType::Unknown };
    String handler_name;
    String language { "und"_string };
    u32 timescale { 1 };
    Optional<u64> duration;
    i64 composition_to_presentation_offset { 0 };
    Vector<SampleEntry> sample_entries;
    TrackFragmentSampleDefaults fragment_defaults;
    u64 sample_table_entry_count { 0 };

    Optional<SampleEntry const&> sample_entry_for_description_index(u32 sample_description_index) const
    {
        if (sample_description_index == 0)
            return {};
        return sample_entries.get(sample_description_index - 1);
    }

    Optional<SampleEntry const&> default_sample_entry() const
    {
        return sample_entry_for_description_index(fragment_defaults.sample_description_index);
    }
};

struct Movie {
    u32 timescale { 1 };
    Optional<u64> duration;
    bool has_movie_extends_box { false };
    OrderedHashMap<u32, NonnullRefPtr<TrackEntry>> tracks;

    Optional<AK::Duration> duration_as_time() const
    {
        if (!duration.has_value())
            return {};
        return AK::Duration::from_time_units(AK::clamp_to<i64>(duration.value()), 1, timescale);
    }
};

struct TrackFragmentContext {
    u32 timescale { 1 };
    i64 composition_to_presentation_offset { 0 };
    size_t sample_description_count { 0 };
    TrackFragmentSampleDefaults sample_defaults;
};

using TrackFragmentContexts = HashMap<u32, TrackFragmentContext>;

static constexpr u32 SAMPLE_IS_NON_SYNC_SAMPLE = 0x00010000;

struct TrackRun {
    u32 track_id { 0 };
    u32 timescale { 1 };
    i64 composition_to_presentation_offset { 0 };
    TrackFragmentSampleDefaults defaults;

    u32 sample_count { 0 };
    size_t data_position { 0 };
    size_t total_data_size { 0 };
    i64 first_decode_time { 0 };
    Optional<u32> first_sample_flags;

    // Each table is populated only when the Track Run Box declares that field, so its size is bounded by the box.
    Vector<u32> sample_durations;
    Vector<u32> sample_sizes;
    Vector<u32> sample_flags;
    Vector<i64> sample_composition_time_offsets;

    size_t data_end() const { return data_position + total_data_size; }

    u32 duration_of_sample(u32 index) const
    {
        return sample_durations.is_empty() ? defaults.sample_duration : sample_durations[index];
    }

    u32 size_of_sample(u32 index) const
    {
        return sample_sizes.is_empty() ? defaults.sample_size : sample_sizes[index];
    }

    u32 flags_of_sample(u32 index) const
    {
        if (!sample_flags.is_empty())
            return sample_flags[index];
        if (index == 0 && first_sample_flags.has_value())
            return first_sample_flags.value();
        return defaults.sample_flags;
    }

    i64 composition_time_offset_of_sample(u32 index) const
    {
        return sample_composition_time_offsets.is_empty() ? 0 : sample_composition_time_offsets[index];
    }
};

struct MovieFragment {
    u32 sequence_number { 0 };
    size_t track_fragment_count { 0 };
    bool uses_movie_fragment_relative_addressing { false };
    bool all_track_fragments_have_decode_times { true };
    Vector<TrackRun> track_runs;

    size_t total_sample_count() const
    {
        size_t total = 0;
        for (auto const& run : track_runs)
            total += run.sample_count;
        return total;
    }
};

struct MovieFragmentAddressing {
    size_t movie_fragment_position { 0 };
    size_t track_fragment_count { 0 };
    size_t previous_track_fragment_data_end { 0 };
    bool all_first_track_runs_have_data_offsets { true };
    bool all_track_fragments_default_base_is_movie_fragment { true };
    bool any_track_fragment_has_base_data_offset { false };
};

inline TrackType track_type_from_handler_type(HandlerType handler_type)
{
    switch (handler_type) {
    case HandlerType::Video:
        return TrackType::Video;
    case HandlerType::Audio:
        return TrackType::Audio;
    case HandlerType::Text:
        return TrackType::Subtitles;
    case HandlerType::Unknown:
        break;
    }
    return TrackType::Unknown;
}

inline Audio::ChannelMap channel_map_from_channel_count(u16 channel_count)
{
    switch (channel_count) {
    case 1:
        return Audio::ChannelMap::mono();
    case 2:
        return Audio::ChannelMap::stereo();
    case 4:
        return Audio::ChannelMap::quadrophonic();
    case 6:
        return Audio::ChannelMap::surround_5_1();
    case 8:
        return Audio::ChannelMap::surround_7_1();
    default:
        return Audio::ChannelMap::invalid();
    }
}

inline Track track_from_track_entry(TrackEntry const& track_entry, bool is_first_of_type)
{
    // https://dev.w3.org/html5/html-sourcing-inband-tracks/#mpeg4
    auto kind = [&] {
        if (track_entry.handler_type == HandlerType::Text) {
            // FIXME: Source "captions" and "subtitles" from the sample entry's format and its WebVTT or TTML
            //        metadata.
            // "metadata": otherwise
            return Track::Kind::Metadata;
        }
        // "main": first audio (video) track
        if (is_first_of_type)
            return Track::Kind::Main;
        // "translation": not first audio (video) track
        return Track::Kind::Translation;
    }();

    // label - Content of the name field in the HandlerBox.
    auto label = Utf16String::from_utf8(track_entry.handler_name);
    // language - Content of the language field in the MediaHeaderBox.
    auto language = Utf16String::from_utf8(track_entry.language);
    Track track(track_type_from_handler_type(track_entry.handler_type), track_entry.track_id, kind, label, language);

    auto sample_entry = track_entry.default_sample_entry();
    if (!sample_entry.has_value())
        return track;

    if (track.type() == TrackType::Video && sample_entry->video.has_value()) {
        auto const& video = sample_entry->video.value();
        track.set_video_data({
            .pixel_width = video.width,
            .pixel_height = video.height,
            .cicp = video.cicp.value_or({}),
        });
    } else if (track.type() == TrackType::Audio && sample_entry->audio.has_value()) {
        auto const& audio = sample_entry->audio.value();
        track.set_audio_data({
            .sample_specification = { audio.sample_rate, channel_map_from_channel_count(audio.channel_count) },
        });
    }

    return track;
}

}
