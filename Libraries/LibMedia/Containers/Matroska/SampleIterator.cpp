/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibMedia/Containers/Matroska/ElementIDs.h>
#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/MediaStream.h>

namespace Media::Matroska {

SampleIterator::SampleIterator(NonnullRefPtr<MediaStreamCursor> const& stream_cursor, Optional<u64> track_number, TrackBlockContexts&& track_contexts, u64 timestamp_scale, size_t segment_contents_position, size_t position)
    : m_stream_cursor(stream_cursor)
    , m_track_number(track_number)
    , m_track_block_contexts(move(track_contexts))
    , m_segment_timestamp_scale(timestamp_scale)
    , m_segment_contents_position(segment_contents_position)
    , m_position(position)
{
}

SampleIterator::~SampleIterator() = default;

DecoderErrorOr<Block> SampleIterator::next_block()
{
    Streamer streamer { m_stream_cursor };
    TRY(streamer.seek_to_position(m_position));

    Optional<Block> block;

    while (true) {
#if MATROSKA_TRACE_DEBUG
        auto element_position = streamer.position();
#endif
        auto element_id = TRY(streamer.read_element_id());
#if MATROSKA_TRACE_DEBUG
        dbgln("Iterator found element with ID {:#010x} at offset {} within the segment.", element_id, element_position);
#endif

        auto maybe_set_block = [&](Block&& candidate_block) {
            if (m_track_number.has_value() && candidate_block.track_number() != m_track_number)
                return;
            block = move(candidate_block);
        };

        if (element_id == CLUSTER_ELEMENT_ID) {
            dbgln_if(MATROSKA_DEBUG, "  Iterator is parsing new cluster.");
            m_current_cluster = TRY(Reader::parse_cluster_element(streamer, m_segment_timestamp_scale));
        } else if (element_id == SIMPLE_BLOCK_ID) {
            if (!m_current_cluster.has_value()) {
                dbgln("  Iterator encountered a simple block before parsing a Cluster.");
                TRY(streamer.read_unknown_element());
            } else {
                dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is parsing a new simple block.");
                auto candidate_block = TRY(Reader::parse_simple_block(streamer, m_current_cluster->timestamp(), m_segment_timestamp_scale, m_track_block_contexts));
                maybe_set_block(move(candidate_block));
            }
        } else if (element_id == BLOCK_GROUP_ID) {
            if (!m_current_cluster.has_value()) {
                dbgln("  Iterator encountered a BlockGroup before parsing a Cluster.");
                TRY(streamer.read_unknown_element());
            } else {
                dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is parsing a new block group.");
                auto candidate_block = TRY(Reader::parse_block_group(streamer, m_current_cluster->timestamp(), m_segment_timestamp_scale, m_track_block_contexts));
                maybe_set_block(move(candidate_block));
            }
        } else if (element_id == SEGMENT_ELEMENT_ID) {
            dbgln("Malformed file, found a segment element within the root segment element. Jumping into it.");
            [[maybe_unused]] auto segment_size = TRY(streamer.read_variable_size_integer());
        } else {
            dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is skipping unknown element with ID {:#010x}.", element_id);
            TRY(streamer.read_unknown_element());
        }

        m_position = streamer.position();
        if (block.has_value()) {
            m_last_timestamp = block->timestamp();
            return block.release_value();
        }
    }

    VERIFY_NOT_REACHED();
}

DecoderErrorOr<Vector<ByteBuffer>> SampleIterator::get_frames(Block block)
{
    Streamer streamer { m_stream_cursor };
    TRY(streamer.seek_to_position(block.data_position()));
    Vector<ByteBuffer> frames;

    if (block.lacing() == Block::Lacing::EBML) {
        auto frames_start_position = streamer.position();
        auto frame_count = TRY(streamer.read_octet()) + 1;
        Vector<u64> frame_sizes;
        frame_sizes.ensure_capacity(frame_count);

        u64 frame_size_sum = 0;
        u64 previous_frame_size;
        auto first_frame_size = TRY(streamer.read_variable_size_integer());
        frame_sizes.append(first_frame_size);
        frame_size_sum += first_frame_size;
        previous_frame_size = first_frame_size;

        for (int i = 0; i < frame_count - 2; i++) {
            auto frame_size_difference = TRY(streamer.read_variable_size_signed_integer());
            u64 frame_size;
            // FIXME: x - (-y) == x + y?
            if (frame_size_difference < 0)
                frame_size = previous_frame_size - (-frame_size_difference);
            else
                frame_size = previous_frame_size + frame_size_difference;
            frame_sizes.append(frame_size);
            frame_size_sum += frame_size;
            previous_frame_size = frame_size;
        }
        frame_sizes.append(block.data_size() - frame_size_sum - (streamer.position() - frames_start_position));

        for (int i = 0; i < frame_count; i++) {
            // FIXME: ReadonlyBytes instead of copying the frame data?
            auto current_frame_size = frame_sizes.at(i);
            frames.append(TRY(streamer.read_raw_octets(current_frame_size)));
        }
    } else if (block.lacing() == Block::Lacing::FixedSize) {
        auto frame_count = TRY(streamer.read_octet()) + 1;
        auto frames_data_size = block.data_size() - 1;
        if ((frames_data_size % frame_count) != 0)
            return DecoderError::corrupted("Block with fixed-size frames has non-divisible size"sv);
        auto individual_frame_size = frames_data_size / frame_count;
        for (int i = 0; i < frame_count; i++)
            frames.append(TRY(streamer.read_raw_octets(individual_frame_size)));
    } else if (block.lacing() == Block::Lacing::XIPH) {
        auto frames_start_position = streamer.position();

        auto frame_count_minus_one = TRY(streamer.read_octet());
        frames.ensure_capacity(frame_count_minus_one + 1);

        auto frame_sizes = Vector<size_t>();
        frame_sizes.ensure_capacity(frame_count_minus_one);
        for (auto i = 0; i < frame_count_minus_one; i++) {
            auto frame_size = 0;
            while (true) {
                auto octet = TRY(streamer.read_octet());
                frame_size += octet;
                if (octet < 255)
                    break;
            }
            frame_sizes.append(frame_size);
        }

        for (auto i = 0; i < frame_count_minus_one; i++)
            frames.append(TRY(streamer.read_raw_octets(frame_sizes[i])));
        frames.append(TRY(streamer.read_raw_octets(block.data_size() - (streamer.position() - frames_start_position))));
    } else {
        frames.append(TRY(streamer.read_raw_octets(block.data_size())));
    }

    return frames;
}

DecoderErrorOr<void> SampleIterator::seek_to_cue_point(TrackCuePoint const& cue_point, CuePointTarget target)
{
    // This is a private function. The position getter can return optional, but the caller should already know that this track has a position.
    auto const& cue_position = cue_point.position;
    Streamer streamer { m_stream_cursor };
    TRY(streamer.seek_to_position(m_segment_contents_position + cue_position.cluster_position()));

    auto element_id = TRY(streamer.read_element_id());
    if (element_id != CLUSTER_ELEMENT_ID)
        return DecoderError::corrupted("Cue point's cluster position didn't point to a cluster"sv);

    m_current_cluster = TRY(Reader::parse_cluster_element(streamer, m_segment_timestamp_scale));
    dbgln_if(MATROSKA_DEBUG, "SampleIterator set to cue point at timestamp {}ms", m_current_cluster->timestamp().to_milliseconds());

    if (target == CuePointTarget::Cluster) {
        m_position = streamer.position();
        m_last_timestamp = m_current_cluster->timestamp();
    } else {
        m_position = streamer.position() + cue_position.block_offset();
        m_last_timestamp = cue_point.timestamp;
    }
    return {};
}

}
