/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/Track.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::Ogg {

static constexpr i64 NO_GRANULE_POSITION = -1;
static constexpr size_t PAGE_HEADER_SIZE = 27;
static constexpr size_t MAXIMUM_SEGMENT_COUNT = 255;
static constexpr size_t MAXIMUM_SEGMENT_SIZE = 255;
static constexpr size_t MAXIMUM_PAGE_SIZE = PAGE_HEADER_SIZE + MAXIMUM_SEGMENT_COUNT + MAXIMUM_SEGMENT_COUNT * MAXIMUM_SEGMENT_SIZE;
// Far beyond any real stream's length, and low enough that times and durations derived from it cannot overflow.
static constexpr i64 MAXIMUM_GRANULE_POSITION = static_cast<i64>(1) << 62;

struct PageHeader {
    size_t position { 0 };
    size_t byte_size { 0 };
    u8 flags { 0 };
    i64 granule_position { NO_GRANULE_POSITION };
    u32 serial_number { 0 };
    u32 sequence_number { 0 };

    size_t end_position() const { return position + byte_size; }
    bool continues_packet() const { return (flags & 0x01) != 0; }
    bool begins_stream() const { return (flags & 0x02) != 0; }
    bool ends_stream() const { return (flags & 0x04) != 0; }
    // A granule position marks the end of the last packet that completes on the page.
    bool has_granule_position() const { return granule_position >= 0 && granule_position <= MAXIMUM_GRANULE_POSITION; }
};

struct PacketFragment {
    ReadonlyBytes bytes;
    bool starts_packet { false };
    bool ends_packet { false };
};

class MEDIA_API PacketFragmentIterator {
public:
    PacketFragmentIterator(ReadonlyBytes segment_sizes, ReadonlyBytes payload, bool continues_packet)
        : m_segment_sizes(segment_sizes)
        , m_payload(payload)
        , m_starts_packet(!continues_packet)
    {
    }

    Optional<PacketFragment> next();

private:
    ReadonlyBytes m_segment_sizes;
    ReadonlyBytes m_payload;
    bool m_starts_packet { false };
};

struct Page {
    PageHeader header;
    FixedArray<u8> segment_sizes;
    FixedArray<u8> payload;

    PacketFragmentIterator packet_fragments() const { return { segment_sizes.span(), payload.span(), header.continues_packet() }; }
    Optional<ReadonlyBytes> first_complete_packet() const;
};

enum class PageReadFailure {
    Incomplete,
    Invalid,
};

using PageHeaderReadResult = Variant<PageHeader, PageReadFailure>;

class MEDIA_API PageIterator {
public:
    PageIterator(NonnullRefPtr<MediaStreamCursor> cursor, size_t position)
        : m_cursor(move(cursor))
        , m_position(position)
    {
    }

    DecoderErrorOr<bool> resync_forward(size_t bound = NumericLimits<size_t>::max());
    // Resynchronizes after invalid pages. Interrupted reads leave the page available for retry.
    DecoderErrorOr<Page> read_next_page();
    // Header-only reads do not validate the payload or CRC.
    DecoderErrorOr<PageHeaderReadResult> read_next_page_header();
    DecoderErrorOr<Optional<Page>> read_page_at(size_t position);
    DecoderErrorOr<Optional<PageHeader>> find_last_page_before(size_t bound, size_t lower_bound);

    size_t position() const { return m_position; }
    void set_position(size_t position) { m_position = position; }
    MediaStreamCursor& cursor() { return *m_cursor; }

private:
    DecoderErrorOr<PageHeaderReadResult> read_header_at(size_t position, FixedArray<u8>* segment_sizes);

    NonnullRefPtr<MediaStreamCursor> m_cursor;
    size_t m_position { 0 };
};

class MEDIA_API PacketAssembler {
public:
    // The packets that complete on the page. A packet continued from a page that was not given is dropped.
    DecoderErrorOr<Vector<FixedArray<u8>>> take_completed_packets(Page const&);
    void reset();

private:
    Optional<ByteBuffer> m_partial_packet;
    Optional<u32> m_expected_sequence_number;
};

struct StreamIdentification {
    CodecID codec_id { CodecID::Unknown };
    TrackType type { TrackType::Audio };
    // Including the identification packet. Zero when the headers last until the first packet that is not one.
    u32 header_packet_count { 0 };

    u32 sample_rate { 0 };
    u8 channel_count { 0 };
    u16 opus_pre_skip { 0 };

    u32 pixel_width { 0 };
    u32 pixel_height { 0 };
    u32 frame_rate_numerator { 0 };
    u32 frame_rate_denominator { 0 };
    u8 theora_keyframe_granule_shift { 0 };
    bool theora_granule_counts_frames_from_one { false };

    Track create_track(size_t identifier) const;

    i64 end_units_of_granule_position(i64 granule_position) const;
    i64 theora_keyframe_start_units(i64 granule_position) const;
    AK::Duration time_of_units(i64 units) const;
    i64 units_of_time(AK::Duration) const;
};

// Empty when the codec is not one we read.
MEDIA_API DecoderErrorOr<Optional<StreamIdentification>> identify_stream(ReadonlyBytes first_packet);
MEDIA_API bool is_header_packet(StreamIdentification const&, ReadonlyBytes packet);
MEDIA_API DecoderErrorOr<FixedArray<u8>> build_codec_initialization_data(StreamIdentification const&, Vector<FixedArray<u8>> const& header_packets);
MEDIA_API bool is_theora_keyframe(ReadonlyBytes packet);

}
