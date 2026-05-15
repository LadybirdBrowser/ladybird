/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/Stream.h>
#include <AK/Time.h>
#include <LibMedia/Containers/ConstantBitrateContainerNavigator.h>
#include <LibMedia/Containers/FLACNavigator.h>
#include <LibMedia/Containers/IndexedContainerNavigator.h>
#include <LibMedia/Containers/MP3Navigator.h>
#include <LibMedia/Containers/OggNavigator.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/SeekMode.h>

extern "C" {
#include <libavformat/avformat.h>
}

namespace Media::FFmpeg {

FFmpegDemuxer::FFmpegDemuxer(NonnullRefPtr<MediaStream> const& stream)
    : m_stream(stream)
{
}

FFmpegDemuxer::~FFmpegDemuxer()
{
    for (auto& [track, context] : m_track_contexts) {
        if (context->format_context != nullptr)
            avformat_close_input(&context->format_context);
    }
}

static DecoderErrorOr<void> initialize_format_context(AVFormatContext*& format_context, AVIOContext& io_context)
{
    format_context = avformat_alloc_context();
    if (format_context == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate format context"sv);
    format_context->pb = &io_context;
    format_context->flags |= AVFMT_FLAG_FAST_SEEK;

    AVDictionary* options = nullptr;
    ScopeGuard free_options = [&] { av_dict_free(&options); };

    // Reduce the maximum packet size for the WAV demuxer, so that playback begins sooner.
    av_dict_set(&options, "max_size", "4096", 0);

    auto open_result = avformat_open_input(&format_context, nullptr, nullptr, &options);
    if (open_result < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to open input for format parsing"sv);

    // Read stream info; doing this is required for headerless formats like MPEG
    if (avformat_find_stream_info(format_context, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to find stream info"sv);

    return {};
}

static DecoderErrorOr<Track> create_track_from_stream(AVStream const& stream, StringView format_name, HashTable<TrackType>& seen_types)
{
    auto type = track_type_from_ffmpeg_media_type(stream.codecpar->codec_type);
    auto get_string_metadata = [&](char const* key) {
        auto* name_entry = av_dict_get(stream.metadata, key, nullptr, 0);
        if (name_entry == nullptr)
            return Utf16String();
        return Utf16String::from_utf8(StringView(name_entry->value, strlen(name_entry->value)));
    };

    // https://dev.w3.org/html5/html-sourcing-inband-tracks/
    auto kind = [&] {
        auto is_first_of_type = seen_types.set(type) == HashSetResult::InsertedNewEntry;
        if (format_name.starts_with("mov"sv)) {
            // https://dev.w3.org/html5/html-sourcing-inband-tracks/#mpeg4avta
            // "main": first audio (video) track
            if (is_first_of_type)
                return Track::Kind::Main;
            // "translation": not first audio (video) track
            return Track::Kind::Translation;
        }

        // AD-HOC: For container formats not covered by the spec, default to "main".
        return Track::Kind::Main;
    }();

    auto name = get_string_metadata("title");
    auto language = get_string_metadata("language");
    Track track(type, stream.index, kind, name, language);

    if (type == TrackType::Video) {
        auto color_primaries = static_cast<ColorPrimaries>(stream.codecpar->color_primaries);
        auto transfer_characteristics = static_cast<TransferCharacteristics>(stream.codecpar->color_trc);
        auto matrix_coefficients = static_cast<MatrixCoefficients>(stream.codecpar->color_space);
        auto color_range = [&stream] {
            switch (stream.codecpar->color_range) {
            case AVColorRange::AVCOL_RANGE_MPEG:
                return VideoFullRangeFlag::Studio;
            case AVColorRange::AVCOL_RANGE_JPEG:
                return VideoFullRangeFlag::Full;
            default:
                return VideoFullRangeFlag::Unspecified;
            }
        }();

        track.set_video_data({
            .pixel_width = static_cast<u64>(stream.codecpar->width),
            .pixel_height = static_cast<u64>(stream.codecpar->height),
            .cicp = CodingIndependentCodePoints(color_primaries, transfer_characteristics, matrix_coefficients, color_range),
        });
    } else if (type == TrackType::Audio) {
        auto channel_map = Audio::ChannelMap::invalid();

        auto& channel_layout = stream.codecpar->ch_layout;
        if (channel_layout.nb_channels != 0) {
            auto channel_map_result = av_channel_layout_to_channel_map(channel_layout);
            if (channel_map_result.is_error())
                return DecoderError::with_description(DecoderErrorCategory::Invalid, channel_map_result.error().string_literal());
            channel_map = channel_map_result.release_value();
        }

        auto sample_specification = Audio::SampleSpecification(stream.codecpar->sample_rate, channel_map);

        track.set_audio_data({
            .sample_specification = sample_specification,
        });
    }

    return track;
}

DecoderErrorOr<NonnullRefPtr<FFmpegDemuxer>> FFmpegDemuxer::from_stream(NonnullRefPtr<MediaStream> const& stream)
{
    auto io_context = DECODER_TRY_ALLOC(Media::FFmpeg::FFmpegIOContext::create(stream->create_cursor()));

    AVFormatContext* format_context = nullptr;
    TRY(initialize_format_context(format_context, *io_context->avio_context()));

    auto demuxer = DECODER_TRY_ALLOC(adopt_nonnull_ref_or_enomem(new (nothrow) FFmpegDemuxer(stream)));
    demuxer->m_total_duration = AK::Duration::from_time_units(format_context->duration, 1, AV_TIME_BASE);
    if (format_context->start_time_realtime != AV_NOPTS_VALUE)
        demuxer->m_start_time_realtime = AK::UnixDateTime::from_microseconds_since_epoch(format_context->start_time_realtime);

    auto format_name = StringView(format_context->iformat->name, strlen(format_context->iformat->name));
    auto seen_types = HashTable<TrackType>();

    for (u32 i = 0; i < format_context->nb_streams; i++) {
        auto& stream = *format_context->streams[i];

        auto track = TRY(create_track_from_stream(stream, format_name, seen_types));
        auto codec_id = media_codec_id_from_ffmpeg_codec_id(stream.codecpar->codec_id);
        auto codec_initialization_data = DECODER_TRY_ALLOC(ByteBuffer::copy(stream.codecpar->extradata, stream.codecpar->extradata_size));

        AK::Duration duration;
        if (stream.duration >= 0)
            duration = AK::Duration::from_time_units(stream.duration, stream.time_base.num, stream.time_base.den);
        else
            duration = demuxer->m_total_duration;

        DECODER_TRY_ALLOC(demuxer->m_stream_info.try_empend(StreamInfo {
            .track = move(track),
            .codec_id = codec_id,
            .codec_initialization_data = move(codec_initialization_data),
            .duration = duration,
            .time_base_numerator = stream.time_base.num,
            .time_base_denominator = stream.time_base.den,
        }));
    }

    demuxer->m_preferred_track_for_type.fill(-1);
    for (u32 i = 0; i < format_context->nb_streams; i++) {
        auto& stream = *format_context->streams[i];
        auto type = track_type_from_ffmpeg_media_type(stream.codecpar->codec_type);
        auto type_index = to_underlying(type);
        if (type_index >= demuxer->m_preferred_track_for_type.size())
            continue;
        if (demuxer->m_preferred_track_for_type[type_index] >= 0)
            continue;
        if (stream.disposition & AV_DISPOSITION_DEFAULT)
            demuxer->m_preferred_track_for_type[type_index] = static_cast<int>(i);
    }

    demuxer->m_container_navigator = create_container_navigator(*format_context, demuxer->m_total_duration, stream);

    avformat_close_input(&format_context);
    return demuxer;
}

static inline AK::Duration time_units_to_duration(i64 time_units, AVRational const& time_base)
{
    VERIFY(time_base.num > 0);
    VERIFY(time_base.den > 0);
    return AK::Duration::from_time_units(time_units, time_base.num, time_base.den);
}

static inline i64 duration_to_time_units(AK::Duration duration, AVRational const& time_base)
{
    VERIFY(time_base.num > 0);
    VERIFY(time_base.den > 0);
    return duration.to_time_units(time_base.num, time_base.den);
}

OwnPtr<ContainerNavigator> FFmpegDemuxer::create_container_navigator(AVFormatContext& context, AK::Duration total_duration, NonnullRefPtr<MediaStream> const& stream)
{
    auto format_name = StringView(context.iformat->name, strlen(context.iformat->name));

    if (format_name == "flac"sv && context.nb_streams == 1) {
        auto& av_stream = *context.streams[0];
        if (av_stream.codecpar->sample_rate > 0) {
            auto sample_rate = static_cast<u32>(av_stream.codecpar->sample_rate);
            AVPacket* packet = av_packet_alloc();
            ScopeGuard free_packet = [&] { av_packet_free(&packet); };

            if (av_read_frame(&context, packet) >= 0) {
                VERIFY(packet->size >= 0);
                auto cursor = stream->create_cursor();
                cursor->set_is_blocking(false);
                return FLACNavigator::create({ packet->data, static_cast<size_t>(packet->size) }, move(cursor), sample_rate);
            }
        }
    }

    if (format_name == "wav"sv && context.nb_streams > 0) {
        auto* stream = context.streams[0];
        auto* codec_par = stream->codecpar;
        if (codec_par->block_align <= 0)
            return nullptr;
        if (codec_par->sample_rate <= 0)
            return nullptr;
        if (Checked<u32>::multiplication_would_overflow(static_cast<u32>(codec_par->block_align), codec_par->sample_rate))
            return nullptr;
        auto bytes_per_second = static_cast<u32>(codec_par->block_align) * codec_par->sample_rate;
        auto entry_count = avformat_index_get_entries_count(stream);
        if (entry_count <= 0)
            return nullptr;
        auto data_offset = avformat_index_get_entry(stream, 0)->pos;
        return make<ConstantBitrateContainerNavigator>(data_offset, bytes_per_second, codec_par->block_align);
    }

    if (format_name == "mp3"sv && context.nb_streams == 1) {
        AVPacket* packet = av_packet_alloc();
        ScopeGuard free_packet = [&] { av_packet_free(&packet); };

        if (av_read_frame(&context, packet) >= 0 && packet->pos >= 0)
            return make<MP3Navigator>(stream, static_cast<size_t>(packet->pos), total_duration);
    }

    if (format_name == "ogg"sv) {
        if (context.nb_streams != 1)
            return nullptr;

        auto& av_stream = *context.streams[0];
        if (av_stream.time_base.num <= 0 || av_stream.time_base.den <= 0)
            return nullptr;

        auto cursor = stream->create_cursor();
        cursor->set_is_blocking(false);

        auto codec_id = FFmpeg::media_codec_id_from_ffmpeg_codec_id(av_stream.codecpar->codec_id);
        ReadonlyBytes first_packet;
        AVPacket* packet = nullptr;
        ScopeGuard free_packet = [&] { av_packet_free(&packet); };

        if (codec_id == CodecID::FLAC) {
            if (av_stream.codecpar->sample_rate <= 0)
                return nullptr;

            packet = av_packet_alloc();
            VERIFY(packet);
            if (av_read_frame(&context, packet) >= 0 && packet->size >= 0)
                first_packet = { packet->data, static_cast<size_t>(packet->size) };
        }

        auto sample_rate = av_stream.codecpar->sample_rate > 0 ? static_cast<u32>(av_stream.codecpar->sample_rate) : 0;
        auto codec_initialization_data = ReadonlyBytes { av_stream.codecpar->extradata, static_cast<size_t>(av_stream.codecpar->extradata_size) };
        return OggNavigator::create(first_packet, move(cursor), codec_id, static_cast<u32>(av_stream.time_base.num), static_cast<u32>(av_stream.time_base.den), sample_rate, codec_initialization_data);
    }

    return create_container_navigator_from_index(context);
}

OwnPtr<ContainerNavigator> FFmpegDemuxer::create_container_navigator_from_index(AVFormatContext& context)
{
    Vector<IndexEntry> entries;
    for (u32 i = 0; i < context.nb_streams; i++) {
        auto* stream = context.streams[i];
        auto entry_count = avformat_index_get_entries_count(stream);
        if (entry_count <= 0)
            continue;

        MUST(entries.try_ensure_capacity(entries.size() + entry_count));
        for (int j = 0; j < entry_count; j++) {
            auto const* entry = avformat_index_get_entry(stream, j);
            entries.unchecked_append({
                .position = static_cast<size_t>(entry->pos),
                .timestamp = time_units_to_duration(entry->timestamp, stream->time_base),
            });
        }
    }

    if (entries.is_empty())
        return nullptr;

    // Sort and ensure monotonic ordering of both positions and timestamps.
    for (size_t i = 1; i < entries.size(); i++) {
        size_t j = i;
        for (; j > 0; --j) {
            if (entries[j - 1].position == entries[j].position)
                return nullptr;
            if (entries[j - 1].position < entries[j].position)
                break;
            swap(entries[j], entries[j - 1]);
        }
        if (j > 0 && entries[j - 1].timestamp > entries[j].timestamp)
            return nullptr;
        if (j < i && entries[j].timestamp > entries[j + 1].timestamp)
            return nullptr;
    }

    auto duration = AK::Duration::from_time_units(context.duration, 1, AV_TIME_BASE);
    return make<IndexedContainerNavigator>(move(entries), duration);
}

DecoderErrorOr<void> FFmpegDemuxer::create_context_for_track(Track const& track)
{
    auto cursor = m_stream->create_cursor();
    auto io_context = MUST(Media::FFmpeg::FFmpegIOContext::create(cursor));

    auto track_context = make<TrackContext>(move(cursor), move(io_context));

    // We've already initialized a format context, so the only way this can fail is OOM.
    MUST(initialize_format_context(track_context->format_context, *track_context->io_context->avio_context()));

    track_context->packet = av_packet_alloc();
    VERIFY(track_context->packet != nullptr);

    VERIFY(m_track_contexts.set(track, move(track_context)) == HashSetResult::InsertedNewEntry);

    return {};
}

FFmpegDemuxer::StreamInfo const& FFmpegDemuxer::get_track_info(Track const& track) const
{
    return m_stream_info[track.identifier()];
}

FFmpegDemuxer::TrackContext& FFmpegDemuxer::get_track_context(Track const& track)
{
    return *m_track_contexts.get(track).release_value();
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::total_duration()
{
    return m_total_duration;
}

Optional<AK::UnixDateTime> FFmpegDemuxer::start_time_realtime() const
{
    return m_start_time_realtime;
}

TimeRanges FFmpegDemuxer::buffered_time_ranges() const
{
    if (!m_container_navigator) {
        TimeRanges ranges;
        if (!m_total_duration.is_zero())
            ranges.add_range(AK::Duration::zero(), m_total_duration);
        return ranges;
    }

    auto byte_ranges = m_stream->available_byte_ranges();
    return m_container_navigator->buffered_time_ranges(byte_ranges);
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::duration_of_track(Track const& track)
{
    auto const& track_info = get_track_info(track);
    return track_info.duration;
}

DecoderErrorOr<Vector<Track>> FFmpegDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    for (auto const& info : m_stream_info) {
        if (info.track.type() == type)
            DECODER_TRY_ALLOC(tracks.try_append(info.track));
    }
    return tracks;
}

DecoderErrorOr<Optional<Track>> FFmpegDemuxer::get_preferred_track_for_type(TrackType type)
{
    auto preferred_index = m_preferred_track_for_type[to_underlying(type)];
    if (preferred_index < 0)
        return OptionalNone();

    return m_stream_info[preferred_index].track;
}

AK::Duration FFmpegDemuxer::select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode)
{
    // FIXME: We can do this using the index getter functions, but unfortunately FFmpeg's seek table is in
    //        DTS -> byte position, so for files with reordered frames (H.264), seeking to a keyframe will often
    //        result in the first frame back being at a later PTS than the seek target, so we would display a blank
    //        frame. To avoid this being especially common, just always accurately seek.
    //
    //        Note that we can end up showing a blank frame anyway by accurately seeking very close to a keyframe,
    //        it's just much less likely to happen under normal usage.
    //
    //        This FIXME can be dropped when MP4/MOV is demuxed separately from FFmpeg, and then inclusion of an
    //        index scan here can be re-evaluated.
    return target;
}

DecoderErrorOr<DemuxerSeekResult> FFmpegDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions)
{
    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;

    VERIFY(track.identifier() < format_context.nb_streams);
    auto& stream = *format_context.streams[track.identifier()];
    auto av_timestamp = duration_to_time_units(timestamp, stream.time_base);

    auto seek_succeeded = false;

    // AVIOContext can skip calling through to the underlying seek callback if the new position lands in its buffer,
    // leaving us in EOF/error, so we need to clear these here.
    format_context.pb->eof_reached = 0;
    format_context.pb->error = 0;

    if (m_container_navigator) {
        auto seek_result = TRY(m_container_navigator->seek_to_timestamp(timestamp));
        if (seek_result.has<SeekSkipped>()) {
            return DemuxerSeekResult::KeptCurrentPosition;
        }
        if (auto const* seeked = seek_result.get_pointer<SeekedPosition>()) {
            if (av_seek_frame(&format_context, stream.index, seeked->byte_position, AVSEEK_FLAG_BYTE) >= 0) {
                seek_succeeded = true;
                track_context.pending_timestamp_offset = seeked->timestamp;
                track_context.timestamp_offset = AK::Duration::zero();
            }
        }
    }

    if (!seek_succeeded) {
        track_context.pending_timestamp_offset.clear();
        track_context.timestamp_offset = AK::Duration::zero();
    }

    if (!seek_succeeded && track_context.is_seekable && av_seek_frame(&format_context, stream.index, av_timestamp, AVSEEK_FLAG_BACKWARD) >= 0)
        seek_succeeded = true;
    if (!seek_succeeded) {
        track_context.is_seekable = false;
        auto av_base_timestamp = duration_to_time_units(timestamp, AV_TIME_BASE_Q);
        if (av_seek_frame(&format_context, -1, av_base_timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            if (track_context.cursor->is_aborted())
                return DecoderError::format(DecoderErrorCategory::Aborted, "Seek aborted");

            return DecoderError::format(DecoderErrorCategory::Corrupted, "Failed to seek");
        }
    }

    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<CodecID> FFmpegDemuxer::get_codec_id_for_track(Track const& track)
{
    auto const& track_info = get_track_info(track);
    return track_info.codec_id;
}

DecoderErrorOr<ReadonlyBytes> FFmpegDemuxer::get_codec_initialization_data_for_track(Track const& track)
{
    auto const& track_info = get_track_info(track);
    return track_info.codec_initialization_data.bytes();
}

DecoderErrorOr<CodedFrame> FFmpegDemuxer::get_next_sample_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;
    auto& packet = *track_context.packet;

    VERIFY(track.identifier() < format_context.nb_streams);
    auto& stream = *format_context.streams[track.identifier()];

    for (;;) {
        auto read_frame_error = av_read_frame(&format_context, &packet);
        if (read_frame_error < 0) {
            if (track_context.cursor->is_aborted())
                return DecoderError::format(DecoderErrorCategory::Aborted, "Read aborted");

            if (read_frame_error == AVERROR_EOF)
                return DecoderError::format(DecoderErrorCategory::EndOfStream, "End of stream");

            return DecoderError::with_description(DecoderErrorCategory::Corrupted, av_error_code_to_string(read_frame_error));
        }
        if (packet.stream_index != stream.index) {
            av_packet_unref(&packet);
            continue;
        }

        auto auxiliary_data = [&]() -> CodedFrame::AuxiliaryData {
            if (track.type() == TrackType::Video) {
                return CodedVideoFrameData();
            }
            if (track.type() == TrackType::Audio) {
                return CodedAudioFrameData();
            }
            VERIFY_NOT_REACHED();
        }();

        // Copy the packet data so that we have a permanent reference to it whilst the Sample is alive, which allows us
        // to wipe the packet afterwards.
        auto packet_data = DECODER_TRY_ALLOC(ByteBuffer::copy(packet.data, packet.size));

        if (track_context.pending_timestamp_offset.has_value() && packet.pts == 0)
            track_context.timestamp_offset = track_context.pending_timestamp_offset.release_value();

        auto flags = (packet.flags & AV_PKT_FLAG_KEY) != 0 ? FrameFlags::Keyframe : FrameFlags::None;
        auto sample = CodedFrame(
            track_context.timestamp_offset + time_units_to_duration(packet.pts, stream.time_base),
            time_units_to_duration(packet.duration, stream.time_base),
            flags,
            move(packet_data),
            auxiliary_data);

        // Wipe the packet now that the data is safe.
        av_packet_unref(&packet);
        return sample;
    }
}

void FFmpegDemuxer::set_blocking_reads_aborted_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    track_context.cursor->abort();
}

void FFmpegDemuxer::reset_blocking_reads_aborted_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    track_context.cursor->reset_abort();
}

bool FFmpegDemuxer::is_read_blocked_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    return track_context.cursor->is_blocked();
}

FFmpegDemuxer::TrackContext::~TrackContext()
{
    av_packet_free(&packet);
    avformat_free_context(format_context);
}

}
