/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/GenericShorthands.h>
#include <AK/SaturatingMath.h>
#include <AK/StringBuilder.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Codecs/Opus.h>

#include "Reader.h"
#include "Streamer.h"

namespace Media::ISOBMFF {

static constexpr u32 TRACK_FRAGMENT_BASE_DATA_OFFSET_PRESENT = 0x000001;
static constexpr u32 TRACK_FRAGMENT_SAMPLE_DESCRIPTION_INDEX_PRESENT = 0x000002;
static constexpr u32 TRACK_FRAGMENT_DEFAULT_SAMPLE_DURATION_PRESENT = 0x000008;
static constexpr u32 TRACK_FRAGMENT_DEFAULT_SAMPLE_SIZE_PRESENT = 0x000010;
static constexpr u32 TRACK_FRAGMENT_DEFAULT_SAMPLE_FLAGS_PRESENT = 0x000020;
static constexpr u32 TRACK_FRAGMENT_DEFAULT_BASE_IS_MOVIE_FRAGMENT = 0x020000;

static constexpr u32 TRACK_RUN_DATA_OFFSET_PRESENT = 0x000001;
static constexpr u32 TRACK_RUN_FIRST_SAMPLE_FLAGS_PRESENT = 0x000004;
static constexpr u32 TRACK_RUN_SAMPLE_DURATION_PRESENT = 0x000100;
static constexpr u32 TRACK_RUN_SAMPLE_SIZE_PRESENT = 0x000200;
static constexpr u32 TRACK_RUN_SAMPLE_FLAGS_PRESENT = 0x000400;
static constexpr u32 TRACK_RUN_SAMPLE_COMPOSITION_TIME_OFFSETS_PRESENT = 0x000800;

static constexpr u8 ELEMENTARY_STREAM_DESCRIPTOR_TAG = 0x03;
static constexpr u8 DECODER_CONFIGURATION_DESCRIPTOR_TAG = 0x04;
static constexpr u8 DECODER_SPECIFIC_INFO_TAG = 0x05;

static constexpr size_t DATA_REFERENCE_IS_SELF_CONTAINED = 0x1;

bool Reader::supports_codec(CodecID codec_id)
{
    return first_is_one_of(codec_id, CodecID::AV1, CodecID::H264, CodecID::H265, CodecID::VP8, CodecID::VP9, CodecID::MP3, CodecID::AAC, CodecID::Opus, CodecID::FLAC);
}

static CodecID codec_id_from_sample_entry_format(FourCC format)
{
    if (format == AV1_SAMPLE_ENTRY)
        return CodecID::AV1;
    if (first_is_one_of(format, AVC_SAMPLE_ENTRY, AVC_INBAND_PARAMETERS_SAMPLE_ENTRY))
        return CodecID::H264;
    if (first_is_one_of(format, HEVC_SAMPLE_ENTRY, HEVC_INBAND_PARAMETERS_SAMPLE_ENTRY))
        return CodecID::H265;
    if (format == VP8_SAMPLE_ENTRY)
        return CodecID::VP8;
    if (format == VP9_SAMPLE_ENTRY)
        return CodecID::VP9;
    if (format == OPUS_SAMPLE_ENTRY)
        return CodecID::Opus;
    if (format == FLAC_SAMPLE_ENTRY)
        return CodecID::FLAC;
    return CodecID::Unknown;
}

static CodecID codec_id_from_object_type_indication(u8 object_type_indication)
{
    switch (object_type_indication) {
    case 0x21:
        return CodecID::H264;
    case 0x23:
        return CodecID::H265;
    case 0x40:
    case 0x66:
    case 0x67:
    case 0x68:
        return CodecID::AAC;
    case 0x69:
    case 0x6B:
        return CodecID::MP3;
    default:
        return CodecID::Unknown;
    }
}

static HandlerType handler_type_from_four_cc(FourCC handler)
{
    if (handler == VIDEO_HANDLER)
        return HandlerType::Video;
    if (handler == AUDIO_HANDLER)
        return HandlerType::Audio;
    if (first_is_one_of(handler, TEXT_HANDLER, SUBTITLE_HANDLER, METADATA_HANDLER))
        return HandlerType::Text;
    return HandlerType::Unknown;
}

DecoderErrorOr<BoxHeader> Reader::read_box_header(Streamer& streamer)
{
    BoxHeader header;
    header.position = streamer.position();

    auto size = TRY(streamer.read<u32>());
    header.type = TRY(streamer.read_four_cc());

    Optional<size_t> total_size;
    if (size == 1)
        total_size = TRY(streamer.read<u64>());
    else if (size != 0)
        total_size = size;

    if (header.type == EXTENDED_TYPE_BOX)
        TRY(streamer.skip(16));

    header.content_position = streamer.position();
    if (total_size.has_value()) {
        auto header_size = header.content_position - header.position;
        if (total_size.value() < header_size)
            return DecoderError::format(DecoderErrorCategory::Corrupted, "{} box is smaller than its own header", header.type);
        if (Checked<size_t>::addition_would_overflow(header.position, total_size.value()))
            return DecoderError::format(DecoderErrorCategory::Corrupted, "{} box size overflows its end position", header.type);
        header.content_size = total_size.value() - header_size;
    }

    dbgln_if(ISOBMFF_TRACE_DEBUG, "Read {} box at {} with {} content bytes", header.type, header.position, header.content_size);
    return header;
}

DecoderErrorOr<FullBoxHeader> Reader::read_full_box_header(Streamer& streamer)
{
    auto version_and_flags = TRY(streamer.read<u32>());
    return FullBoxHeader {
        .version = static_cast<u8>(version_and_flags >> 24),
        .flags = version_and_flags & 0xFFFFFF,
    };
}

DecoderErrorOr<void> Reader::parse_child_boxes(Streamer& streamer, BoxHeader const& parent, IsTopLevel is_top_level, Function<DecoderErrorOr<IterationDecision>(BoxHeader const&)> child_consumer)
{
    auto parent_end = parent.end_position();
    if (!parent_end.has_value() && is_top_level == IsTopLevel::No)
        return DecoderError::corrupted("Boxes may only extend to the end of the file at the top level"sv);
    if (parent_end.has_value() && streamer.position() > parent_end.value())
        return DecoderError::format(DecoderErrorCategory::Corrupted, "Read past the end of the {} box", parent.type);

    while (!parent_end.has_value() || streamer.position() < parent_end.value()) {
        auto child_or_error = read_box_header(streamer);
        if (child_or_error.is_error()) {
            if (!parent_end.has_value() && child_or_error.error().category() == DecoderErrorCategory::EndOfStream)
                break;
            return child_or_error.release_error();
        }

        auto child = child_or_error.release_value();
        auto child_end = child.end_position();
        if (!child_end.has_value())
            return DecoderError::format(DecoderErrorCategory::Corrupted, "{} box may only extend to the end of the file at the top level, not within a {} box", child.type, parent.type);
        if (parent_end.has_value() && child_end.value() > parent_end.value())
            return DecoderError::format(DecoderErrorCategory::Corrupted, "{} box extends past the end of its parent {} box", child.type, parent.type);

        auto decision = TRY(child_consumer(child));
        if (streamer.position() > child_end.value())
            return DecoderError::format(DecoderErrorCategory::Corrupted, "Read past the end of the {} box", child.type);
        TRY(streamer.seek_to_position(child_end.value()));
        if (decision == IterationDecision::Break)
            break;
    }

    return {};
}

DecoderErrorOr<FileType> Reader::parse_file_type_box(Streamer& streamer, BoxHeader const& header)
{
    auto end = header.end_position();
    if (!end.has_value())
        return DecoderError::corrupted("File Type box must declare a size"sv);
    if (header.content_size.value() < 2 * sizeof(u32) || (header.content_size.value() - 2 * sizeof(u32)) % sizeof(u32) != 0)
        return DecoderError::corrupted("File Type box has an invalid size"sv);

    FileType file_type;
    file_type.major_brand = TRY(streamer.read_four_cc());
    file_type.minor_version = TRY(streamer.read<u32>());
    while (streamer.position() + sizeof(u32) <= end.value())
        DECODER_TRY_ALLOC(file_type.compatible_brands.try_append(TRY(streamer.read_four_cc())));

    dbgln_if(ISOBMFF_DEBUG, "Read {} box with major brand {}", header.type, file_type.major_brand);
    return file_type;
}

DecoderErrorOr<Movie> Reader::parse_movie_box(Streamer& streamer, BoxHeader const& header)
{
    Movie movie;
    HashMap<u32, TrackFragmentSampleDefaults> track_extends_defaults;
    Optional<u64> fragment_duration;

    TRY(parse_child_boxes(streamer, header, IsTopLevel::Yes, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        if (child.type == MOVIE_HEADER_BOX) {
            auto full_box = TRY(read_full_box_header(streamer));
            if (full_box.version == 0) {
                // creation_time and modification_time
                TRY(streamer.skip(8));
                movie.timescale = TRY(streamer.read<u32>());
                auto duration = TRY(streamer.read<u32>());
                if (duration != NumericLimits<u32>::max())
                    movie.duration = duration;
            } else if (full_box.version == 1) {
                // creation_time and modification_time
                TRY(streamer.skip(16));
                movie.timescale = TRY(streamer.read<u32>());
                auto duration = TRY(streamer.read<u64>());
                if (duration != NumericLimits<u64>::max())
                    movie.duration = duration;
            } else {
                return DecoderError::corrupted("Movie Header box has an unsupported version"sv);
            }
            if (movie.timescale == 0)
                return DecoderError::corrupted("Movie Header box has a zero timescale"sv);
        } else if (child.type == TRACK_BOX) {
            auto track = TRY(parse_track_box(streamer, child));
            auto track_id = track->track_id;
            if (movie.tracks.set(track_id, move(track)) != HashSetResult::InsertedNewEntry)
                return DecoderError::format(DecoderErrorCategory::Corrupted, "Found a duplicate track ID {}", track_id);
        } else if (child.type == MOVIE_EXTENDS_BOX) {
            movie.has_movie_extends_box = true;
            TRY(parse_child_boxes(streamer, child, IsTopLevel::No, [&](BoxHeader const& extends_child) -> DecoderErrorOr<IterationDecision> {
                if (extends_child.type == MOVIE_EXTENDS_HEADER_BOX) {
                    auto full_box = TRY(read_full_box_header(streamer));
                    if (full_box.version == 0) {
                        fragment_duration = TRY(streamer.read<u32>());
                    } else if (full_box.version == 1) {
                        fragment_duration = TRY(streamer.read<u64>());
                    } else {
                        return DecoderError::corrupted("Movie Extends Header box has an unsupported version"sv);
                    }
                } else if (extends_child.type == TRACK_EXTENDS_BOX) {
                    TRY(read_full_box_header(streamer));
                    auto track_id = TRY(streamer.read<u32>());
                    TrackFragmentSampleDefaults defaults;
                    defaults.sample_description_index = TRY(streamer.read<u32>());
                    defaults.sample_duration = TRY(streamer.read<u32>());
                    defaults.sample_size = TRY(streamer.read<u32>());
                    defaults.sample_flags = TRY(streamer.read<u32>());
                    DECODER_TRY_ALLOC(track_extends_defaults.try_set(track_id, defaults));
                }
                return IterationDecision::Continue;
            }));
        }
        return IterationDecision::Continue;
    }));

    for (auto& [track_id, track] : movie.tracks) {
        auto defaults = track_extends_defaults.get(track_id);
        if (defaults.has_value())
            track->fragment_defaults = defaults.release_value();
        if (!track->sample_entries.is_empty() && !track->default_sample_entry().has_value())
            return DecoderError::format(DecoderErrorCategory::Corrupted, "Track {} has an invalid default sample description index", track_id);
    }

    // Fragmented files may carry their duration in the Movie Extends Header instead.
    if (movie.duration.value_or(0) == 0)
        movie.duration = fragment_duration;

    return movie;
}

DecoderErrorOr<NonnullRefPtr<TrackEntry>> Reader::parse_track_box(Streamer& streamer, BoxHeader const& header)
{
    auto track = DECODER_TRY_ALLOC(try_make_ref_counted<TrackEntry>());
    bool found_track_header = false;

    TRY(parse_child_boxes(streamer, header, IsTopLevel::No, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        if (child.type == TRACK_HEADER_BOX) {
            auto full_box = TRY(read_full_box_header(streamer));
            track->is_enabled = (full_box.flags & 1) != 0;
            if (full_box.version == 0) {
                // creation_time and modification_time
                TRY(streamer.skip(8));
            } else if (full_box.version == 1) {
                // creation_time and modification_time
                TRY(streamer.skip(16));
            } else {
                return DecoderError::corrupted("Track Header box has an unsupported version"sv);
            }
            track->track_id = TRY(streamer.read<u32>());
            found_track_header = true;
        } else if (child.type == EDIT_BOX) {
            TRY(parse_child_boxes(streamer, child, IsTopLevel::No, [&](BoxHeader const& edit_child) -> DecoderErrorOr<IterationDecision> {
                if (edit_child.type == EDIT_LIST_BOX)
                    track->composition_to_presentation_offset = TRY(parse_edit_list_box(streamer, edit_child));
                return IterationDecision::Continue;
            }));
        } else if (child.type == MEDIA_BOX) {
            TRY(parse_media_box(streamer, child, *track));
        }
        return IterationDecision::Continue;
    }));

    if (!found_track_header)
        return DecoderError::corrupted("Track box contains no Track Header box"sv);

    dbgln_if(ISOBMFF_DEBUG, "Read track {} with codec {}", track->track_id, track->sample_entries.is_empty() ? CodecID::Unknown : track->sample_entries[0].codec_id);
    return track;
}

DecoderErrorOr<i64> Reader::parse_edit_list_box(Streamer& streamer, BoxHeader const& header)
{
    auto full_box = TRY(read_full_box_header(streamer));
    i64 media_time = 0;
    if (full_box.version == 0) {
        auto entry_count = TRY(streamer.read<u32>());
        if (entry_count == 0)
            return 0;
        if (entry_count != 1)
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Edit lists with multiple entries are not supported"sv);
        TRY(streamer.skip(4)); // segment_duration
        media_time = TRY(streamer.read<i32>());
    } else if (full_box.version == 1) {
        auto entry_count = TRY(streamer.read<u32>());
        if (entry_count == 0)
            return 0;
        if (entry_count != 1)
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Edit lists with multiple entries are not supported"sv);
        TRY(streamer.skip(8)); // segment_duration
        media_time = TRY(streamer.read<i64>());
    } else {
        return DecoderError::corrupted("Edit List box has an unsupported version"sv);
    }
    auto media_rate_integer = TRY(streamer.read<i16>());
    auto media_rate_fraction = TRY(streamer.read<i16>());

    if (media_rate_integer != 1 || media_rate_fraction != 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Edit list media rates other than one are not supported"sv);
    if (media_time < 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Empty edits are not supported"sv);

    if (streamer.position() > header.end_position().value())
        return DecoderError::corrupted("Edit List box is smaller than the entry it declares"sv);
    return -media_time;
}

DecoderErrorOr<void> Reader::parse_media_box(Streamer& streamer, BoxHeader const& header, TrackEntry& track)
{
    return parse_child_boxes(streamer, header, IsTopLevel::No, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        if (child.type == MEDIA_HEADER_BOX) {
            auto full_box = TRY(read_full_box_header(streamer));
            if (full_box.version == 0) {
                // creation_time and modification_time
                TRY(streamer.skip(8));
                track.timescale = TRY(streamer.read<u32>());
                auto duration = TRY(streamer.read<u32>());
                if (duration != NumericLimits<u32>::max())
                    track.duration = duration;
            } else if (full_box.version == 1) {
                // creation_time and modification_time
                TRY(streamer.skip(16));
                track.timescale = TRY(streamer.read<u32>());
                auto duration = TRY(streamer.read<u64>());
                if (duration != NumericLimits<u64>::max())
                    track.duration = duration;
            } else {
                return DecoderError::corrupted("Media Header box has an unsupported version"sv);
            }
            if (track.timescale == 0)
                return DecoderError::corrupted("Media Header box has a zero timescale"sv);

            // The language uses three packed five-bit characters, each offset from 0x60.
            auto packed_language = TRY(streamer.read<u16>());
            StringBuilder language;
            bool language_is_valid = true;
            for (size_t index = 0; index < 3; index++) {
                auto character = static_cast<char>(0x60 + ((packed_language >> ((2 - index) * 5)) & 0x1F));
                language_is_valid &= character >= 'a' && character <= 'z';
                language.append(character);
            }
            if (language_is_valid)
                track.language = DECODER_TRY_ALLOC(language.to_string());
        } else if (child.type == HANDLER_BOX) {
            TRY(read_full_box_header(streamer));
            TRY(streamer.skip(4)); // pre_defined
            track.handler_type = handler_type_from_four_cc(TRY(streamer.read_four_cc()));
            TRY(streamer.skip(12)); // reserved
            if (streamer.position() > child.end_position().value())
                return DecoderError::corrupted("Handler box is smaller than its own header"sv);
            track.handler_name = TRY(streamer.read_null_terminated_string(child.end_position().value() - streamer.position()));
        } else if (child.type == MEDIA_INFORMATION_BOX) {
            TRY(parse_child_boxes(streamer, child, IsTopLevel::No, [&](BoxHeader const& information_child) -> DecoderErrorOr<IterationDecision> {
                if (information_child.type == DATA_INFORMATION_BOX)
                    TRY(parse_data_information_box(streamer, information_child));
                else if (information_child.type == SAMPLE_TABLE_BOX)
                    TRY(parse_sample_table_box(streamer, information_child, track));
                return IterationDecision::Continue;
            }));
        }
        return IterationDecision::Continue;
    });
}

DecoderErrorOr<void> Reader::parse_data_information_box(Streamer& streamer, BoxHeader const& header)
{
    return parse_child_boxes(streamer, header, IsTopLevel::No, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        if (child.type != DATA_REFERENCE_BOX)
            return IterationDecision::Continue;

        TRY(read_full_box_header(streamer));
        auto entry_count = TRY(streamer.read<u32>());
        for (u32 index = 0; index < entry_count; index++) {
            if (streamer.position() >= child.end_position().value())
                return DecoderError::corrupted("Data Reference box is too small for the number of entries it declares"sv);
            auto entry = TRY(read_box_header(streamer));
            auto entry_end = entry.end_position();
            if (!entry_end.has_value())
                return DecoderError::corrupted("Data reference entry must declare a size"sv);
            if (entry_end.value() > child.end_position().value())
                return DecoderError::corrupted("Data reference entry extends past the end of the Data Reference box"sv);
            auto entry_full_box = TRY(read_full_box_header(streamer));
            if ((entry_full_box.flags & DATA_REFERENCE_IS_SELF_CONTAINED) == 0)
                return DecoderError::corrupted("Track refers to media data in another file"sv);
            TRY(streamer.seek_to_position(entry_end.value()));
        }
        return IterationDecision::Continue;
    });
}

DecoderErrorOr<void> Reader::parse_sample_table_box(Streamer& streamer, BoxHeader const& header, TrackEntry& track)
{
    return parse_child_boxes(streamer, header, IsTopLevel::No, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        if (child.type == SAMPLE_DESCRIPTION_BOX) {
            track.sample_entries = TRY(parse_sample_description_box(streamer, child, track.handler_type));
        } else if (first_is_one_of(child.type, TIME_TO_SAMPLE_BOX, SAMPLE_TO_CHUNK_BOX, CHUNK_OFFSET_BOX, CHUNK_LARGE_OFFSET_BOX)) {
            // FIXME: Parse the sample tables so that unfragmented files can be demuxed. For now we only record
            //        whether they describe any samples at all.
            TRY(read_full_box_header(streamer));
            track.sample_table_entry_count += static_cast<u64>(TRY(streamer.read<u32>()));
        }
        return IterationDecision::Continue;
    });
}

DecoderErrorOr<Vector<SampleEntry>> Reader::parse_sample_description_box(Streamer& streamer, BoxHeader const& header, HandlerType handler_type)
{
    TRY(read_full_box_header(streamer));
    auto entry_count = TRY(streamer.read<u32>());
    if (streamer.position() > header.end_position().value())
        return DecoderError::corrupted("Sample Description box is smaller than its header"sv);
    auto remaining_size = header.end_position().value() - streamer.position();
    if (entry_count > remaining_size / (sizeof(u32) + sizeof(FourCC)))
        return DecoderError::corrupted("Sample Description box is too small for the number of entries it declares"sv);
    Vector<SampleEntry> entries;
    DECODER_TRY_ALLOC(entries.try_ensure_capacity(entry_count));
    for (u32 index = 0; index < entry_count; ++index) {
        auto entry_header = TRY(read_box_header(streamer));
        auto entry_end = entry_header.end_position();
        if (!entry_end.has_value() || entry_end.value() > header.end_position().value())
            return DecoderError::corrupted("Sample entry extends past the end of the Sample Description box"sv);
        DECODER_TRY_ALLOC(entries.try_append(TRY(parse_sample_entry(streamer, entry_header, handler_type))));
        TRY(streamer.seek_to_position(entry_end.value()));
    }
    return entries;
}

DecoderErrorOr<SampleEntry> Reader::parse_sample_entry(Streamer& streamer, BoxHeader const& header, HandlerType handler_type)
{
    SampleEntry entry;
    entry.format = header.type;
    entry.codec_id = codec_id_from_sample_entry_format(header.type);

    TRY(streamer.skip(6)); // reserved
    TRY(streamer.skip(2)); // data_reference_index

    if (handler_type == HandlerType::Video) {
        TRY(streamer.skip(2 + 2 + 12)); // pre_defined, reserved, pre_defined
        VideoSampleData video;
        video.width = TRY(streamer.read<u16>());
        video.height = TRY(streamer.read<u16>());
        // horizresolution, vertresolution, reserved, frame_count, compressorname, depth, pre_defined
        TRY(streamer.skip(4 + 4 + 4 + 2 + 32 + 2 + 2));
        entry.video = video;
    } else if (handler_type == HandlerType::Audio) {
        auto version = TRY(streamer.read<u16>());
        TRY(streamer.skip(6)); // reserved
        AudioSampleData audio;
        audio.channel_count = TRY(streamer.read<u16>());
        audio.bits_per_sample = TRY(streamer.read<u16>());
        TRY(streamer.skip(2 + 2)); // pre_defined, reserved
        audio.sample_rate = static_cast<u32>(TRY(streamer.read_fixed_point_16_16()));
        if (version == 0) {
            entry.audio = audio;
        } else if (version == 1) {
            TRY(streamer.skip(16));
            entry.audio = audio;
        } else if (version == 2) {
            auto sample_entry_fields_size = TRY(streamer.read<u32>());
            auto sample_rate = bit_cast<double>(TRY(streamer.read<u64>()));
            auto channel_count = TRY(streamer.read<u32>());
            TRY(streamer.skip(4));
            auto bits_per_sample = TRY(streamer.read<u32>());
            TRY(streamer.skip(12));

            if (!isfinite(sample_rate) || sample_rate < 0 || sample_rate > NumericLimits<u32>::max())
                return DecoderError::corrupted("Audio sample rate is invalid"sv);
            if (channel_count > NumericLimits<u16>::max() || bits_per_sample > NumericLimits<u16>::max())
                return DecoderError::corrupted("Audio sample format is invalid"sv);
            audio.sample_rate = static_cast<u32>(sample_rate);
            audio.channel_count = static_cast<u16>(channel_count);
            audio.bits_per_sample = static_cast<u16>(bits_per_sample);

            static constexpr size_t MINIMUM_VERSION_2_SAMPLE_ENTRY_SIZE = 72;
            if (sample_entry_fields_size < MINIMUM_VERSION_2_SAMPLE_ENTRY_SIZE)
                return DecoderError::corrupted("Version 2 audio sample entry fields are too small"sv);
            if (Checked<size_t>::addition_would_overflow(header.position, sample_entry_fields_size))
                return DecoderError::corrupted("Version 2 audio sample entry fields are too large"sv);
            auto child_boxes_position = header.position + sample_entry_fields_size;
            if (child_boxes_position > header.end_position().value())
                return DecoderError::corrupted("Version 2 audio sample entry fields extend past the end of the box"sv);
            TRY(streamer.seek_to_position(child_boxes_position));
            entry.audio = audio;
        } else {
            return DecoderError::corrupted("Audio sample entry has an unsupported version"sv);
        }
    } else {
        // We cannot parse further for formats we don't understand, as the child boxes may be preceded by fields we
        // don't know how to parse (yet).
        return entry;
    }

    TRY(parse_child_boxes(streamer, header, IsTopLevel::No, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        auto read_configuration = [&] -> DecoderErrorOr<FixedArray<u8>> {
            return streamer.read_bytes(child.content_size.value());
        };

        if (first_is_one_of(child.type, AVC_CONFIGURATION_BOX, HEVC_CONFIGURATION_BOX, AV1_CONFIGURATION_BOX)) {
            entry.codec_initialization_data = TRY(read_configuration());
        } else if (child.type == OPUS_CONFIGURATION_BOX) {
            auto configuration = TRY(read_configuration());
            entry.codec_initialization_data = TRY(Codecs::Opus::codec_initialization_data_from_isobmff_configuration(configuration.span()));
        } else if (child.type == FLAC_CONFIGURATION_BOX) {
            auto configuration = TRY(read_configuration());
            entry.codec_initialization_data = TRY(Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(configuration.span()));
        } else if (child.type == ELEMENTARY_STREAM_DESCRIPTOR_BOX) {
            entry.codec_initialization_data = TRY(parse_elementary_stream_descriptor_box(streamer, child, entry.codec_id));
        } else if (child.type == COLOUR_INFORMATION_BOX && entry.video.has_value()) {
            if (TRY(streamer.read_four_cc()) == NCLX_COLOUR_TYPE) {
                auto colour_primaries = TRY(streamer.read<u16>());
                auto transfer_characteristics = TRY(streamer.read<u16>());
                auto matrix_coefficients = TRY(streamer.read<u16>());
                auto is_full_range = (TRY(streamer.read<u8>()) & 0x80) != 0;
                entry.video->cicp = CodingIndependentCodePoints {
                    static_cast<ColorPrimaries>(colour_primaries),
                    static_cast<TransferCharacteristics>(transfer_characteristics),
                    static_cast<MatrixCoefficients>(matrix_coefficients),
                    is_full_range ? VideoFullRangeFlag::Full : VideoFullRangeFlag::Studio,
                };
            }
        }
        return IterationDecision::Continue;
    }));

    return entry;
}

static DecoderErrorOr<u32> read_descriptor_size(Streamer& streamer)
{
    u32 size = 0;
    for (size_t index = 0; index < 4; index++) {
        auto byte = TRY(streamer.read<u8>());
        size = (size << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0)
            break;
    }
    return size;
}

DecoderErrorOr<FixedArray<u8>> Reader::parse_elementary_stream_descriptor_box(Streamer& streamer, BoxHeader const& header, CodecID& codec_id)
{
    TRY(read_full_box_header(streamer));

    if (TRY(streamer.read<u8>()) != ELEMENTARY_STREAM_DESCRIPTOR_TAG)
        return DecoderError::corrupted("Elementary stream descriptor box contains no ES_Descriptor"sv);
    TRY(read_descriptor_size(streamer));
    TRY(streamer.skip(2)); // ES_ID
    auto flags = TRY(streamer.read<u8>());
    if ((flags & 0x80) != 0)
        TRY(streamer.skip(2)); // dependsOn_ES_ID
    if ((flags & 0x40) != 0) {
        auto url_length = TRY(streamer.read<u8>()); // URLlength
        if (streamer.position() + url_length > header.end_position().value())
            return DecoderError::corrupted("ES_Descriptor's URL extends past the end of its box"sv);
        TRY(streamer.skip(url_length)); // URLstring
    }
    if ((flags & 0x20) != 0)
        TRY(streamer.skip(2)); // OCR_ES_Id

    if (TRY(streamer.read<u8>()) != DECODER_CONFIGURATION_DESCRIPTOR_TAG)
        return DecoderError::corrupted("ES_Descriptor contains no DecoderConfigDescriptor"sv);
    TRY(read_descriptor_size(streamer));
    codec_id = codec_id_from_object_type_indication(TRY(streamer.read<u8>()));
    // streamType, upStream and reserved, bufferSizeDB, maxBitrate, avgBitrate
    TRY(streamer.skip(1 + 3 + 4 + 4));

    auto end = header.end_position().value();
    if (streamer.position() >= end || TRY(streamer.read<u8>()) != DECODER_SPECIFIC_INFO_TAG)
        return FixedArray<u8> {};
    auto size = TRY(read_descriptor_size(streamer));
    if (streamer.position() + size > end)
        return DecoderError::corrupted("DecoderSpecificInfo extends past the end of its box"sv);
    return streamer.read_bytes(size);
}

DecoderErrorOr<MovieFragment> Reader::parse_movie_fragment_box(Streamer& streamer, BoxHeader const& header, TrackFragmentContexts const& contexts)
{
    MovieFragment fragment;
    MovieFragmentAddressing addressing { .movie_fragment_position = header.position };
    bool found_movie_fragment_header = false;

    TRY(parse_child_boxes(streamer, header, IsTopLevel::Yes, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        if (child.type == MOVIE_FRAGMENT_HEADER_BOX) {
            if (found_movie_fragment_header)
                return DecoderError::corrupted("Movie Fragment box contains multiple Movie Fragment Header boxes"sv);
            auto full_box = TRY(read_full_box_header(streamer));
            if (full_box.version == 0) {
                fragment.sequence_number = TRY(streamer.read<u32>());
            } else {
                return DecoderError::corrupted("Movie Fragment Header box has an unsupported version"sv);
            }
            found_movie_fragment_header = true;
        } else if (child.type == TRACK_FRAGMENT_BOX) {
            if (!found_movie_fragment_header)
                return DecoderError::corrupted("Track Fragment box appeared before the Movie Fragment Header box"sv);
            TRY(parse_track_fragment_box(streamer, child, addressing, contexts, fragment));
        }
        return IterationDecision::Continue;
    }));

    if (!found_movie_fragment_header)
        return DecoderError::corrupted("Movie Fragment box contains no Movie Fragment Header box"sv);

    fragment.track_fragment_count = addressing.track_fragment_count;
    // https://w3c.github.io/mse-byte-stream-format-isobmff/#iso-media-segments
    // A Movie Fragment Box uses movie-fragment relative addressing when the first Track Fragment Run (trun) box
    // in each Track Fragment Box has the data-offset-present flag set and either of the following conditions
    // are met:
    //   - Every Track Fragment Box in a Movie Fragment Box has the default-base-is-moof flag set.
    //   - The Movie Fragment Box contains a single Track Fragment Box and that box does not have the
    //     base-data-offset-present flag set.
    fragment.uses_movie_fragment_relative_addressing = addressing.all_first_track_runs_have_data_offsets
        && (addressing.all_track_fragments_default_base_is_movie_fragment
            || (addressing.track_fragment_count == 1 && !addressing.any_track_fragment_has_base_data_offset));

    dbgln_if(ISOBMFF_DEBUG, "Read movie fragment {} with {} samples across {} track fragments", fragment.sequence_number, fragment.total_sample_count(), fragment.track_fragment_count);
    return fragment;
}

DecoderErrorOr<void> Reader::parse_track_fragment_box(Streamer& streamer, BoxHeader const& header, MovieFragmentAddressing& addressing, TrackFragmentContexts const& contexts, MovieFragment& fragment)
{
    auto is_first_track_fragment = addressing.track_fragment_count == 0;
    addressing.track_fragment_count++;

    u32 track_id = 0;
    TrackFragmentContext context;
    TrackFragmentSampleDefaults defaults;
    size_t base_data_position = addressing.movie_fragment_position;
    size_t next_track_run_position = base_data_position;
    i64 next_sample_decode_time = 0;
    bool found_track_fragment_header = false;
    bool found_decode_time = false;
    bool is_first_track_run = true;

    TRY(parse_child_boxes(streamer, header, IsTopLevel::No, [&](BoxHeader const& child) -> DecoderErrorOr<IterationDecision> {
        if (child.type == TRACK_FRAGMENT_HEADER_BOX) {
            if (found_track_fragment_header)
                return DecoderError::corrupted("Track Fragment box contains multiple Track Fragment Header boxes"sv);
            auto full_box = TRY(read_full_box_header(streamer));
            if (full_box.version == 0) {
                track_id = TRY(streamer.read<u32>());
            } else {
                return DecoderError::corrupted("Track Fragment Header box has an unsupported version"sv);
            }

            auto maybe_context = contexts.get(track_id);
            if (!maybe_context.has_value())
                return DecoderError::format(DecoderErrorCategory::Corrupted, "Track fragment refers to unknown track ID {}", track_id);
            context = maybe_context.release_value();
            defaults = context.sample_defaults;

            auto has_base_data_offset = (full_box.flags & TRACK_FRAGMENT_BASE_DATA_OFFSET_PRESENT) != 0;
            auto default_base_is_movie_fragment = !has_base_data_offset && (full_box.flags & TRACK_FRAGMENT_DEFAULT_BASE_IS_MOVIE_FRAGMENT) != 0;
            addressing.all_track_fragments_default_base_is_movie_fragment &= default_base_is_movie_fragment;

            if (has_base_data_offset) {
                addressing.any_track_fragment_has_base_data_offset = true;
                base_data_position = TRY(streamer.read<u64>());
            } else if (default_base_is_movie_fragment || is_first_track_fragment) {
                base_data_position = addressing.movie_fragment_position;
            } else {
                base_data_position = addressing.previous_track_fragment_data_end;
            }
            next_track_run_position = base_data_position;

            if ((full_box.flags & TRACK_FRAGMENT_SAMPLE_DESCRIPTION_INDEX_PRESENT) != 0)
                defaults.sample_description_index = TRY(streamer.read<u32>());
            if ((full_box.flags & TRACK_FRAGMENT_DEFAULT_SAMPLE_DURATION_PRESENT) != 0)
                defaults.sample_duration = TRY(streamer.read<u32>());
            if ((full_box.flags & TRACK_FRAGMENT_DEFAULT_SAMPLE_SIZE_PRESENT) != 0)
                defaults.sample_size = TRY(streamer.read<u32>());
            if ((full_box.flags & TRACK_FRAGMENT_DEFAULT_SAMPLE_FLAGS_PRESENT) != 0)
                defaults.sample_flags = TRY(streamer.read<u32>());
            if (defaults.sample_description_index == 0 || defaults.sample_description_index > context.sample_description_count)
                return DecoderError::corrupted("Track fragment has an invalid sample description index"sv);

            found_track_fragment_header = true;
        } else if (child.type == TRACK_FRAGMENT_DECODE_TIME_BOX) {
            if (!found_track_fragment_header)
                return DecoderError::corrupted("Track Fragment Decode Time box appeared before the Track Fragment Header box"sv);
            if (found_decode_time)
                return DecoderError::corrupted("Track Fragment box contains multiple Track Fragment Decode Time boxes"sv);
            auto full_box = TRY(read_full_box_header(streamer));
            i64 base_media_decode_time = 0;
            if (full_box.version == 0) {
                base_media_decode_time = TRY(streamer.read<u32>());
            } else if (full_box.version == 1) {
                // AD-HOC: This field is supposed to be unsigned, but FFmpeg's MP4 encoder (movenc.c) writes it as
                //         signed.
                base_media_decode_time = static_cast<i64>(TRY(streamer.read<u64>()));
            } else {
                return DecoderError::corrupted("Track Fragment Decode Time box has an unsupported version"sv);
            }
            next_sample_decode_time = base_media_decode_time;
            found_decode_time = true;
        } else if (child.type == TRACK_RUN_BOX) {
            if (!found_track_fragment_header)
                return DecoderError::corrupted("Track Run box appeared before the Track Fragment Header box"sv);

            auto full_box = TRY(read_full_box_header(streamer));
            bool has_signed_composition_time_offsets;
            if (full_box.version == 0) {
                has_signed_composition_time_offsets = false;
            } else if (full_box.version == 1) {
                has_signed_composition_time_offsets = true;
            } else {
                return DecoderError::corrupted("Track Run box has an unsupported version"sv);
            }
            if ((full_box.flags & TRACK_RUN_FIRST_SAMPLE_FLAGS_PRESENT) != 0 && (full_box.flags & TRACK_RUN_SAMPLE_FLAGS_PRESENT) != 0)
                return DecoderError::corrupted("Track Run box has conflicting sample flags"sv);
            auto sample_count = TRY(streamer.read<u32>());

            auto has_data_offset = (full_box.flags & TRACK_RUN_DATA_OFFSET_PRESENT) != 0;
            if (is_first_track_run) {
                addressing.all_first_track_runs_have_data_offsets &= has_data_offset;
                is_first_track_run = false;
            }

            auto track_run_position = next_track_run_position;
            if (has_data_offset) {
                auto data_offset = TRY(streamer.read<i32>());
                if (data_offset < 0) {
                    auto magnitude = static_cast<u64>(-static_cast<i64>(data_offset));
                    if (magnitude > base_data_position)
                        return DecoderError::corrupted("Track run data offset points before the start of the stream"sv);
                    track_run_position = base_data_position - static_cast<size_t>(magnitude);
                } else {
                    if (Checked<size_t>::addition_would_overflow(base_data_position, static_cast<u32>(data_offset)))
                        return DecoderError::corrupted("Track run data offset is too large"sv);
                    track_run_position = base_data_position + static_cast<u32>(data_offset);
                }
            }

            Optional<u32> first_sample_flags;
            if ((full_box.flags & TRACK_RUN_FIRST_SAMPLE_FLAGS_PRESENT) != 0)
                first_sample_flags = TRY(streamer.read<u32>());

            auto track_run_end = child.end_position().value();
            if (streamer.position() > track_run_end)
                return DecoderError::corrupted("Track Run box is smaller than its own header"sv);

            auto has_sample_durations = (full_box.flags & TRACK_RUN_SAMPLE_DURATION_PRESENT) != 0;
            auto has_sample_sizes = (full_box.flags & TRACK_RUN_SAMPLE_SIZE_PRESENT) != 0;
            auto has_sample_flags = (full_box.flags & TRACK_RUN_SAMPLE_FLAGS_PRESENT) != 0;
            auto has_composition_time_offsets = (full_box.flags & TRACK_RUN_SAMPLE_COMPOSITION_TIME_OFFSETS_PRESENT) != 0;

            size_t bytes_per_sample = 0;
            for (auto is_present : { has_sample_durations, has_sample_sizes, has_sample_flags, has_composition_time_offsets }) {
                if (is_present)
                    bytes_per_sample += sizeof(u32);
            }
            if (static_cast<u64>(sample_count) * bytes_per_sample > track_run_end - streamer.position())
                return DecoderError::corrupted("Track Run box is too small for the number of samples it declares"sv);

            TrackRun run;
            run.track_id = track_id;
            run.timescale = context.timescale;
            run.composition_to_presentation_offset = context.composition_to_presentation_offset;
            run.defaults = defaults;
            run.sample_count = sample_count;
            run.data_position = track_run_position;
            run.first_decode_time = next_sample_decode_time;
            run.first_sample_flags = first_sample_flags;

            if (has_sample_durations)
                DECODER_TRY_ALLOC(run.sample_durations.try_ensure_capacity(sample_count));
            if (has_sample_sizes)
                DECODER_TRY_ALLOC(run.sample_sizes.try_ensure_capacity(sample_count));
            if (has_sample_flags)
                DECODER_TRY_ALLOC(run.sample_flags.try_ensure_capacity(sample_count));
            if (has_composition_time_offsets)
                DECODER_TRY_ALLOC(run.sample_composition_time_offsets.try_ensure_capacity(sample_count));

            u64 total_data_size = 0;
            u64 total_duration = 0;
            if (bytes_per_sample == 0) {
                // Every sample takes its fields from the defaults, so the run is described without reading any.
                total_data_size = static_cast<u64>(sample_count) * defaults.sample_size;
                total_duration = static_cast<u64>(sample_count) * defaults.sample_duration;
            } else {
                for (u32 index = 0; index < sample_count; index++) {
                    auto duration = defaults.sample_duration;
                    if (has_sample_durations) {
                        duration = TRY(streamer.read<u32>());
                        run.sample_durations.unchecked_append(duration);
                    }
                    auto size = defaults.sample_size;
                    if (has_sample_sizes) {
                        size = TRY(streamer.read<u32>());
                        run.sample_sizes.unchecked_append(size);
                    }
                    if (has_sample_flags)
                        run.sample_flags.unchecked_append(TRY(streamer.read<u32>()));
                    if (has_composition_time_offsets) {
                        if (has_signed_composition_time_offsets)
                            run.sample_composition_time_offsets.unchecked_append(TRY(streamer.read<i32>()));
                        else
                            run.sample_composition_time_offsets.unchecked_append(TRY(streamer.read<u32>()));
                    }
                    total_data_size += size;
                    total_duration += duration;
                }
            }

            if (total_data_size > NumericLimits<size_t>::max() - track_run_position)
                return DecoderError::corrupted("Sample data position is too large"sv);
            run.total_data_size = static_cast<size_t>(total_data_size);

            next_track_run_position = run.data_end();
            next_sample_decode_time = saturating_add(next_sample_decode_time, AK::clamp_to<i64>(total_duration));
            DECODER_TRY_ALLOC(fragment.track_runs.try_append(move(run)));
        }
        return IterationDecision::Continue;
    }));

    if (!found_track_fragment_header)
        return DecoderError::corrupted("Track Fragment box contains no Track Fragment Header box"sv);

    addressing.previous_track_fragment_data_end = next_track_run_position;
    fragment.all_track_fragments_have_decode_times &= found_decode_time;
    return {};
}

}
