/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <LibMedia/Containers/Ogg/OggDemuxer.h>
#include <LibMediaCodecs/CodedFrame.h>

namespace Media::Ogg {

static constexpr size_t MAXIMUM_SEEK_PROBE_COUNT = 16;
static constexpr size_t MAXIMUM_RESYNC_BYTES_PER_SCAN = 4 * MiB;
// Header packets may contain large cover images.
static constexpr size_t MAXIMUM_LINK_HEADER_BYTE_COUNT = 64 * MiB;
static constexpr size_t MAXIMUM_LINK_STREAM_COUNT = 64;
static constexpr size_t MAXIMUM_SCANNED_STREAM_COUNT = 16 * KiB;
// The last pages of a file may belong to streams without a time, such as one we don't read, so a few are searched.
static constexpr size_t MAXIMUM_TAIL_PROBE_WINDOW_COUNT = 4;
// Opus needs this much audio decoded ahead of a position before its output converges there.
static constexpr AK::Duration OPUS_SEEK_PREROLL = AK::Duration::from_milliseconds(80);
static constexpr AK::Duration SEEK_LEAD = AK::Duration::from_milliseconds(500);

bool OggDemuxer::supports_container_mime_type(ContainerMimeType mime_type)
{
    return mime_type.container_id == ContainerID::Ogg;
}

bool OggDemuxer::supports_codec_in_container(ContainerID container_id, CodecID codec_id)
{
    return container_id == ContainerID::Ogg && first_is_one_of(codec_id, CodecID::Vorbis, CodecID::Opus, CodecID::FLAC, CodecID::Theora);
}

DecoderErrorOr<OggDemuxer::Link> OggDemuxer::Link::clone() const
{
    Link link;
    link.position = position;
    for (auto const& stream : streams)
        DECODER_TRY_ALLOC(link.streams.try_append({ { stream.serial_number, stream.identification }, stream.beginning_page, DECODER_TRY_ALLOC(stream.codec_initialization_data.clone()) }));
    return link;
}

Optional<size_t> OggDemuxer::Link::index_of_serial_number(u32 serial_number) const
{
    for (size_t i = 0; i < streams.size(); i++) {
        if (streams[i].serial_number == serial_number)
            return i;
    }
    return {};
}

template<typename Streams>
static Optional<size_t> index_of_stream_of_type(Streams const& streams, TrackType type, size_t index_within_type)
{
    size_t index = 0;
    for (size_t i = 0; i < streams.size(); i++) {
        auto const& identification = streams[i].identification;
        if (!identification.has_value() || identification->type != type)
            continue;
        if (index++ == index_within_type)
            return i;
    }
    return {};
}

Optional<size_t> OggDemuxer::Link::index_of_stream_for_track(TrackType type, size_t index_within_type) const
{
    return index_of_stream_of_type(streams, type, index_within_type);
}

Optional<size_t> OggDemuxer::ScannedLink::index_of_stream_for_track(TrackType type, size_t index_within_type) const
{
    return index_of_stream_of_type(streams, type, index_within_type);
}

DecoderErrorOr<void> OggDemuxer::StreamHeaderReader::consume(Page const& page)
{
    if (page.header.begins_stream()) {
        stream.serial_number = page.header.serial_number;
        stream.beginning_page = page.header;
        if (auto packet = page.first_complete_packet(); packet.has_value()) {
            auto identification = identify_stream(*packet);
            if (identification.is_error()) {
                if (identification.error().category() != DecoderErrorCategory::Corrupted)
                    return identification.release_error();
            } else {
                stream.identification = identification.release_value();
            }
        }
    }
    if (!needs_more_pages())
        return {};

    auto completed_packets = assembler.take_completed_packets(page);
    if (completed_packets.is_error()) {
        if (completed_packets.error().category() != DecoderErrorCategory::Corrupted)
            return completed_packets.release_error();
        stream.identification = {};
        return {};
    }
    auto const& identification = *stream.identification;
    for (auto& packet : completed_packets.value()) {
        if (identification.header_packet_count == 0 && !is_header_packet(identification, packet.span())) {
            complete = true;
            break;
        }
        packets.append(move(packet));
        if (identification.header_packet_count != 0 && packets.size() >= identification.header_packet_count) {
            complete = true;
            break;
        }
    }
    if (complete) {
        auto data = build_codec_initialization_data(identification, packets);
        if (data.is_error()) {
            if (data.error().category() != DecoderErrorCategory::Corrupted)
                return data.release_error();
            stream.identification = {};
        } else {
            stream.codec_initialization_data = data.release_value();
        }
        packets.clear();
        assembler.reset();
    }
    return {};
}

DecoderErrorOr<OggDemuxer::Link> OggDemuxer::read_link(PageIterator& pages)
{
    Link link;
    Vector<StreamHeaderReader> header_readers;
    bool reading_stream_starts = true;
    auto needs_more_headers = [&] {
        return any_of(header_readers, [](auto const& reader) { return reader.needs_more_pages(); });
    };
    while (reading_stream_starts || needs_more_headers()) {
        if (!header_readers.is_empty() && pages.position() - link.position >= MAXIMUM_LINK_HEADER_BYTE_COUNT)
            break;
        auto page_or_error = pages.read_next_page();
        if (page_or_error.is_error()) {
            if (page_or_error.error().category() == DecoderErrorCategory::EndOfStream && !header_readers.is_empty())
                break;
            return page_or_error.release_error();
        }
        auto page = page_or_error.release_value();
        if (page.header.begins_stream()) {
            if (!reading_stream_starts) {
                pages.set_position(page.header.position);
                break;
            }
            if (header_readers.is_empty())
                link.position = page.header.position;
            if (header_readers.size() == MAXIMUM_LINK_STREAM_COUNT || any_of(header_readers, [&](auto const& reader) { return reader.stream.serial_number == page.header.serial_number; }))
                return DecoderError::corrupted("Ogg link announces too many streams"sv);
            StreamHeaderReader reader;
            TRY(reader.consume(page));
            header_readers.append(move(reader));
        } else {
            reading_stream_starts = false;
            for (auto& reader : header_readers) {
                if (reader.stream.serial_number == page.header.serial_number && reader.needs_more_pages())
                    TRY(reader.consume(page));
            }
        }
    }
    if (header_readers.is_empty())
        return DecoderError::corrupted("No Ogg stream begins here"sv);
    for (auto& reader : header_readers) {
        if (!reader.complete)
            reader.stream.identification = {};
        link.streams.append(move(reader.stream));
    }
    return link;
}

namespace {

struct TailProbe {
    AK::Duration duration;
    Optional<u32> serial_number;
    size_t position { 0 };
    i64 end_units { 0 };
};

}

// The duration is where the last page of the file ends. A page of a stream the first link never announced means the
// file is chained, whose duration can only be learned by reading every link.
template<typename Link>
static DecoderErrorOr<TailProbe> probe_tail(MediaStreamCursor& cursor, Link const& link, size_t stream_size)
{
    // The last page ends the file, so it starts within a page's reach of the end. Each window is read in one go, so
    // that a backward search through it asks for its data once.
    auto bound = stream_size;
    auto window_end = stream_size;
    for (size_t window = 0; window < MAXIMUM_TAIL_PROBE_WINDOW_COUNT && window_end > link.position; window++) {
        auto window_start = max(link.position, window_end > MAXIMUM_PAGE_SIZE ? window_end - MAXIMUM_PAGE_SIZE : 0);
        auto window_data = DECODER_TRY_ALLOC(ByteBuffer::create_uninitialized(window_end - window_start));
        TRY(cursor.seek_to_position(window_start));
        if (auto result = cursor.read_into(window_data.bytes()); result.is_error() && result.error().category() != DecoderErrorCategory::EndOfStream)
            return result.release_error();

        PageIterator tail { cursor, window_start };
        while (true) {
            auto header = TRY(tail.find_last_page_before(bound, window_start));
            if (!header.has_value())
                break;
            auto index = link.index_of_serial_number(header->serial_number);
            if (!index.has_value())
                return TailProbe {};
            auto const& identification = link.streams[*index].identification;
            if (identification.has_value() && header->has_granule_position()) {
                auto end_units = identification->end_units_of_granule_position(header->granule_position);
                return TailProbe { identification->time_of_units(end_units), header->serial_number, header->position, end_units };
            }
            bound = header->position;
        }
        window_end = window_start;
    }
    return TailProbe {};
}

DecoderErrorOr<NonnullRefPtr<Demuxer>> OggDemuxer::from_stream(NonnullRefPtr<MediaStream> const& stream)
{
    auto cursor = stream->create_cursor();
    PageIterator pages { cursor, 0 };
    auto first_page = TRY(pages.read_page_at(0));
    if (!first_page.has_value() || !first_page->header.begins_stream())
        return DecoderError::with_description(DecoderErrorCategory::UnrecognizedFormat, "Stream is not Ogg"sv);

    auto link = TRY(read_link(pages));

    Vector<TrackDescription> tracks;
    for (auto const& link_stream : link.streams) {
        if (!link_stream.identification.has_value())
            continue;
        auto type = link_stream.identification->type;
        size_t index_within_type = 0;
        for (auto const& track : tracks) {
            if (track.track.type() == type)
                index_within_type++;
        }
        tracks.append({ link_stream.identification->create_track(tracks.size()), index_within_type });
    }
    if (tracks.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Ogg stream has no logical stream we can read"sv);

    TailProbe tail;
    if (auto stream_size = stream->expected_size(); stream_size.has_value())
        tail = TRY(probe_tail(*cursor, link, *stream_size));
    Optional<PageAnchor> tail_anchor;
    if (tail.serial_number.has_value())
        tail_anchor = PageAnchor { *tail.serial_number, tail.position, tail.end_units };

    auto demuxer = adopt_ref(*new OggDemuxer(stream, move(link), move(tracks), tail.duration, tail_anchor));
    demuxer->start_buffered_scan_thread();
    return demuxer;
}

OggDemuxer::OggDemuxer(NonnullRefPtr<MediaStream> const& stream, Link first_link, Vector<TrackDescription> tracks, AK::Duration duration, Optional<PageAnchor> tail_anchor)
    : m_stream(stream)
    , m_first_link(move(first_link))
    , m_tracks(move(tracks))
    , m_duration(duration)
    , m_tail_anchor(tail_anchor)
{
    m_track_statuses.resize(m_tracks.size());
}

OggDemuxer::~OggDemuxer()
{
    if (m_buffered_scan_thread != nullptr)
        m_buffered_scan_thread->shutdown();
}

static i64 packet_duration_in_units(Codecs::AudioPacketDurationParser& packet_duration_parser, StreamIdentification const& identification, ReadonlyBytes packet)
{
    if (identification.codec_id == CodecID::Theora)
        return 1;
    auto duration = packet_duration_parser.parse_duration_in_samples(packet);
    return duration.is_error() ? 0 : duration.value();
}

i64 OggDemuxer::BufferedScanPayload::start_units_of_page(PageHeader const& header, ScannedStream const& stream, i64 end_units)
{
    auto const& identification = *stream.description.identification;
    // Without the codec's headers, only the end of the page is known to be decodable.
    if (identification.codec_id != CodecID::Theora && !stream.packet_duration_parser.parses_durations())
        return end_units;
    auto packet_duration_parser = stream.packet_duration_parser;
    packet_duration_parser.reset();

    auto page = walker.read_page_at(header.position);
    if (page.is_error() || !page.value().has_value())
        return end_units;

    i64 duration_units = 0;
    auto fragments = page.value()->packet_fragments();
    for (auto fragment = fragments.next(); fragment.has_value(); fragment = fragments.next()) {
        if (fragment->starts_packet && fragment->ends_packet && !is_header_packet(identification, fragment->bytes))
            duration_units += packet_duration_in_units(packet_duration_parser, identification, fragment->bytes);
    }
    return end_units - duration_units;
}

void OggDemuxer::BufferedScanPayload::scan_page(ScannedByteRange& range, PageHeader const& header)
{
    if (header.begins_stream()) {
        auto stream = streams.find(header.serial_number);
        if (stream == streams.end()) {
            if (streams.size() >= MAXIMUM_SCANNED_STREAM_COUNT)
                return;
            ScannedStream scanned_stream {
                .description = { header.serial_number, {} },
                .beginning_page_position = header.position,
                .pending_headers = StreamHeaderReader {},
                .next_header_page_position = header.position,
            };
            streams.set(header.serial_number, move(scanned_stream));
            stream = streams.find(header.serial_number);
        }
        if (stream->value.beginning_page_position == header.position && range.previous_page.has_value())
            stream->value.preceding_page = range.previous_page;
        return;
    }
    if (!header.has_granule_position())
        return;
    for (auto& extent : range.extents) {
        if (extent.first_page.serial_number == header.serial_number) {
            extent.last_page = header;
            return;
        }
    }
    if (range.extents.size() < MAXIMUM_SCANNED_STREAM_COUNT)
        range.extents.append({ header, header });
}

void OggDemuxer::BufferedScanPayload::walk(ScannedByteRange& range)
{
    if (!range.next_page_position.has_value()) {
        range.next_page_position = range.byte_start;
        range.needs_resync = true;
        range.previous_page = {};
    }
    while (*range.next_page_position < range.byte_end) {
        walker.set_position(*range.next_page_position);
        if (range.needs_resync) {
            auto bound = min(range.byte_end, *range.next_page_position + MAXIMUM_RESYNC_BYTES_PER_SCAN);
            auto found = walker.resync_forward(bound);
            if (found.is_error())
                return;
            if (!found.value()) {
                // Retain enough overlap to retry a page cut off by the buffered range.
                if (bound == *range.next_page_position + MAXIMUM_RESYNC_BYTES_PER_SCAN)
                    range.next_page_position = bound - MAXIMUM_PAGE_SIZE;
                return;
            }
            range.next_page_position = walker.position();
            range.needs_resync = false;
            range.previous_page = {};
        }

        auto result = walker.read_next_page_header();
        if (result.is_error())
            return;
        if (auto const* failure = result.value().get_pointer<PageReadFailure>()) {
            if (*failure == PageReadFailure::Incomplete)
                return;
            ++*range.next_page_position;
            range.needs_resync = true;
            continue;
        }
        auto const& header = result.value().get<PageHeader>();
        if (header.end_position() > range.byte_end)
            return;
        scan_page(range, header);
        range.previous_page = header;
        range.next_page_position = header.end_position();
    }
}

void OggDemuxer::BufferedScanPayload::read_pending_headers(ScannedStream& stream) const
{
    if (!stream.pending_headers.has_value())
        return;
    auto& reader = *stream.pending_headers;
    PageIterator pages = walker;
    pages.set_position(stream.next_header_page_position);
    while (pages.position() - stream.beginning_page_position < MAXIMUM_LINK_HEADER_BYTE_COUNT) {
        auto result = pages.read_next_page_header();
        if (result.is_error())
            return;
        if (auto const* failure = result.value().get_pointer<PageReadFailure>()) {
            if (*failure == PageReadFailure::Incomplete)
                return;
            break;
        }
        auto const& header = result.value().get<PageHeader>();
        if (header.begins_stream() && header.position != stream.beginning_page_position && reader.stream.serial_number == header.serial_number)
            break;
        if (header.serial_number == stream.description.serial_number) {
            auto page = pages.read_page_at(header.position);
            if (page.is_error() || !page.value().has_value())
                return;
            if (reader.consume(*page.value()).is_error())
                break;
            if (!reader.needs_more_pages()) {
                stream.description.identification = reader.stream.identification;
                if (stream.description.identification.has_value())
                    stream.packet_duration_parser = Codecs::AudioPacketDurationParser::create(stream.description.identification->codec_id, reader.stream.codec_initialization_data.span()).value_or({});
                break;
            }
        }
        stream.next_header_page_position = pages.position();
    }
    stream.pending_headers = {};
}

Vector<OggDemuxer::ScannedLink> OggDemuxer::BufferedScanPayload::assemble_links(size_t first_link_position) const
{
    Vector<ScannedStream const*> ordered_streams;
    for (auto const& entry : streams)
        ordered_streams.append(&entry.value);
    quick_sort(ordered_streams, [](auto* a, auto* b) { return a->beginning_page_position < b->beginning_page_position; });

    Vector<ScannedLink> links;
    HashMap<u32, size_t> link_indices;
    for (auto const* stream : ordered_streams) {
        Optional<size_t> concurrent_link_index;
        if (stream->preceding_page.has_value() && stream->preceding_page->begins_stream())
            concurrent_link_index = link_indices.get(stream->preceding_page->serial_number);
        if (!concurrent_link_index.has_value() || links[*concurrent_link_index].streams.size() >= MAXIMUM_LINK_STREAM_COUNT) {
            ScannedLink link;
            link.position = stream->beginning_page_position;
            if (stream->preceding_page.has_value() && !stream->preceding_page->begins_stream())
                link.preceding_stream_serial_number = stream->preceding_page->serial_number;
            links.append(move(link));
            concurrent_link_index = links.size() - 1;
        }
        auto& link = links[*concurrent_link_index];
        link.streams.append(stream->description);
        link_indices.set(stream->description.serial_number, *concurrent_link_index);
        if (stream->description.identification.has_value() && stream->maximum_granule_position.has_value()) {
            auto const& identification = *stream->description.identification;
            auto end_time = identification.time_of_units(identification.end_units_of_granule_position(*stream->maximum_granule_position));
            link.end_time = max(link.end_time.value_or(AK::Duration::zero()), end_time);
        }
    }
    for (size_t i = 0; i < links.size(); i++) {
        auto& link = links[i];
        if (link.position == first_link_position) {
            link.start_offset = AK::Duration::zero();
            continue;
        }
        if (!link.preceding_stream_serial_number.has_value())
            continue;
        auto preceding_index = link_indices.get(*link.preceding_stream_serial_number);
        if (!preceding_index.has_value() || *preceding_index >= i)
            continue;
        auto const& preceding = links[*preceding_index];
        if (preceding.start_offset.has_value() && preceding.end_time.has_value())
            link.start_offset = *preceding.start_offset + *preceding.end_time;
    }
    return links;
}

DemuxerScanState OggDemuxer::BufferedScanPayload::scan_available_ranges(MediaStream::Availability const& availability, ReadonlySpan<TrackDescription> tracks, size_t first_link_position, AK::Duration minimum_duration)
{
    scanned_ranges.update(availability.byte_ranges, [&](ScannedByteRange& range, CachedRangeChange change) {
        if (has_flag(change, CachedRangeChange::Start) || range.next_page_position.value_or(range.byte_start) > range.byte_end) {
            range.next_page_position = {};
            range.extents.clear();
        }
        walk(range);
    });
    for (auto& entry : streams)
        read_pending_headers(entry.value);
    for (auto const& range : scanned_ranges.ranges()) {
        for (auto const& extent : range.extents) {
            auto stream = streams.find(extent.last_page.serial_number);
            if (stream != streams.end())
                stream->value.maximum_granule_position = max(stream->value.maximum_granule_position.value_or(0), extent.last_page.granule_position);
        }
    }
    auto links = assemble_links(first_link_position);
    HashMap<u32, size_t> link_indices;
    for (size_t i = 0; i < links.size(); i++) {
        for (auto const& stream : links[i].streams)
            link_indices.set(stream.serial_number, i);
    }

    Vector<BufferedRangesScan> scans;
    for (auto const& track : tracks)
        scans.append({ .time_ranges = {}, .last_byte_range_has_samples = false, .track_identifier = track.track.identifier() });
    Vector<PageAnchor> anchors;
    for (auto const& range : scanned_ranges.ranges()) {
        Vector<Optional<AK::Duration>> starts;
        Vector<Optional<AK::Duration>> ends;
        starts.resize(tracks.size());
        ends.resize(tracks.size());
        for (auto const& extent : range.extents) {
            auto serial_number = extent.first_page.serial_number;
            auto stream_entry = streams.find(serial_number);
            if (stream_entry == streams.end() || !stream_entry->value.description.identification.has_value())
                continue;
            auto const& stream = stream_entry->value;
            auto const& identification = *stream.description.identification;
            auto const& link = links[link_indices.get(serial_number).value()];
            auto first_end_units = identification.end_units_of_granule_position(extent.first_page.granule_position);
            auto last_end_units = identification.end_units_of_granule_position(extent.last_page.granule_position);
            anchors.append({ serial_number, extent.first_page.position, first_end_units });
            anchors.append({ serial_number, extent.last_page.position, last_end_units });
            if (!link.start_offset.has_value())
                continue;
            auto first_start_units = start_units_of_page(extent.first_page, stream, first_end_units);
            for (size_t track_index = 0; track_index < tracks.size(); track_index++) {
                auto const& track = tracks[track_index];
                auto stream_index = link.index_of_stream_for_track(track.track.type(), track.index_within_type);
                if (!stream_index.has_value() || link.streams[*stream_index].serial_number != serial_number)
                    continue;
                auto start = *link.start_offset + max(identification.time_of_units(first_start_units), AK::Duration::zero());
                auto end = *link.start_offset + identification.time_of_units(last_end_units);
                if (!starts[track_index].has_value() || start < *starts[track_index])
                    starts[track_index] = start;
                if (!ends[track_index].has_value() || end > *ends[track_index])
                    ends[track_index] = end;
            }
        }
        for (size_t track_index = 0; track_index < tracks.size(); track_index++) {
            auto& scan = scans[track_index];
            auto has_samples = starts[track_index].has_value() && *starts[track_index] < *ends[track_index];
            scan.last_byte_range_has_samples = has_samples;
            if (has_samples)
                scan.time_ranges.add_range(*starts[track_index], *ends[track_index]);
        }
    }

    {
        MutexLocker locker { published->mutex };
        published->links = move(links);
        published->anchors = move(anchors);
    }
    return DemuxerScanState::create_from_track_scans(move(scans), minimum_duration, availability.closing_bytes_are_available());
}

void OggDemuxer::start_buffered_scan_thread()
{
    HashMap<u32, ScannedStream> streams;
    Optional<PageHeader> preceding_page;
    for (auto const& stream : m_first_link.streams) {
        ScannedStream scanned_stream {
            .description = { stream.serial_number, stream.identification },
            .beginning_page_position = stream.beginning_page.position,
            .preceding_page = preceding_page,
        };
        if (stream.identification.has_value())
            scanned_stream.packet_duration_parser = Codecs::AudioPacketDurationParser::create(stream.identification->codec_id, stream.codec_initialization_data.span()).value_or({});
        streams.set(stream.serial_number, move(scanned_stream));
        preceding_page = stream.beginning_page;
    }

    auto scan_cursor = m_stream->create_cursor();
    scan_cursor->set_is_blocking(false);
    BufferedScanPayload payload {
        .walker = PageIterator { move(scan_cursor), 0 },
        .scanned_ranges = {},
        .streams = move(streams),
        .published = m_published_scan,
    };
    m_published_scan->links = payload.assemble_links(m_first_link.position);

    DemuxerScanState initial_state;
    initial_state.duration = m_duration;
    m_buffered_scan_thread = DemuxerScanThread<BufferedScanPayload>::start(m_stream, move(initial_state), move(payload),
        [tracks = m_tracks, duration = m_duration, first_link_position = m_first_link.position](MediaStream::Availability const& availability, BufferedScanPayload& payload) {
            return payload.scan_available_ranges(availability, tracks, first_link_position, duration);
        });
}

OggDemuxer::TrackDescription const& OggDemuxer::track_description(Track const& track) const
{
    VERIFY(track.identifier() < m_tracks.size());
    auto const& description = m_tracks[track.identifier()];
    VERIFY(description.track == track);
    return description;
}

OggDemuxer::TrackStatus& OggDemuxer::track_status(Track const& track)
{
    MutexLocker locker { m_track_statuses_mutex };
    VERIFY(track.identifier() < m_track_statuses.size());
    auto& status = m_track_statuses[track.identifier()];
    VERIFY(status != nullptr);
    return *status;
}

DecoderErrorOr<void> OggDemuxer::enter_link(TrackStatus& status, TrackDescription const& description, Link link, AK::Duration start_offset)
{
    Codecs::AudioPacketDurationParser packet_duration_parser;
    auto stream_index = link.index_of_stream_for_track(description.track.type(), description.index_within_type);
    if (stream_index.has_value()) {
        auto const& stream = link.streams[*stream_index];
        packet_duration_parser = TRY(Codecs::AudioPacketDurationParser::create(stream.identification->codec_id, stream.codec_initialization_data.span()));
    }

    status.link = move(link);
    status.stream_index = stream_index;
    status.link_start_offset = start_offset;
    status.link_end_time = {};
    status.packet_duration_parser = packet_duration_parser;
    status.assembler.reset();
    status.running_end_units = {};
    status.needs_codec_configuration = true;
    status.reading_seek_preroll_page = false;
    status.ready_frames.clear();
    return {};
}

DecoderErrorOr<void> OggDemuxer::create_context_for_track(Track const& track)
{
    auto const& description = track_description(track);
    auto cursor = m_stream->create_cursor();
    cursor->set_is_blocking(true);

    auto status = make<TrackStatus>(PageIterator { move(cursor), m_first_link.position });
    TRY(enter_link(*status, description, TRY(m_first_link.clone()), AK::Duration::zero()));

    MutexLocker locker { m_track_statuses_mutex };
    VERIFY(m_track_statuses[track.identifier()] == nullptr);
    m_track_statuses[track.identifier()] = move(status);
    return {};
}

DecoderErrorOr<Vector<Track>> OggDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    for (auto const& description : m_tracks) {
        if (description.track.type() == type)
            tracks.append(description.track);
    }
    return tracks;
}

DecoderErrorOr<Optional<Track>> OggDemuxer::get_preferred_track_for_type(TrackType type)
{
    for (auto const& description : m_tracks) {
        if (description.track.type() == type)
            return Optional<Track> { description.track };
    }
    return Optional<Track> {};
}

DecoderErrorOr<void> OggDemuxer::queue_packets(TrackStatus& status, PageHeader const& header, Vector<FixedArray<u8>> packets)
{
    auto const& stream = status.link.streams[*status.stream_index];
    auto const& identification = *stream.identification;

    packets.remove_all_matching([&](auto const& packet) { return is_header_packet(identification, packet.span()); });

    Vector<i64> durations;
    for (auto const& packet : packets)
        durations.append(packet_duration_in_units(status.packet_duration_parser, identification, packet.span()));

    // A page's granule position is where its last packet ends, which places every packet that completes on it.
    // The final page of a stream may end before its last packet does, trimming that packet's end, so it is placed
    // from the packets before it where they are known.
    Vector<i64> ends;
    ends.resize(packets.size());
    i64 trailing_trim_units = 0;
    auto place_packet_ends_from_start_units = [&](i64 start) {
        for (size_t i = 0; i < packets.size(); i++) {
            start += durations[i];
            ends[i] = start;
        }
    };
    if (header.has_granule_position()) {
        auto page_end = identification.end_units_of_granule_position(header.granule_position);
        if (header.ends_stream() && status.running_end_units.has_value() && !packets.is_empty()) {
            place_packet_ends_from_start_units(*status.running_end_units);
            if (ends.last() > page_end)
                trailing_trim_units = min(ends.last() - page_end, durations.last());
        } else {
            auto end = page_end;
            for (size_t i = packets.size(); i-- > 0;) {
                ends[i] = end;
                end -= durations[i];
            }
        }
        status.running_end_units = page_end;
    } else if (status.running_end_units.has_value()) {
        place_packet_ends_from_start_units(*status.running_end_units);
        if (!packets.is_empty())
            status.running_end_units = ends.last();
    } else {
        return {};
    }
    if (status.reading_seek_preroll_page) {
        status.reading_seek_preroll_page = false;
        return {};
    }

    for (size_t i = 0; i < packets.size(); i++) {
        auto& packet = packets[i];
        auto flags = FrameFlags::Keyframe;
        if (identification.codec_id == CodecID::Theora) {
            // An empty packet repeats the frame before it, which the sink holds anyway.
            if (packet.is_empty())
                continue;
            if (!is_theora_keyframe(packet.span())) {
                if (status.waiting_for_keyframe)
                    continue;
                flags = FrameFlags::None;
            }
            status.waiting_for_keyframe = false;
        }

        auto start_time = identification.time_of_units(ends[i] - durations[i]);
        auto full_end_time = identification.time_of_units(ends[i]);
        auto end_time = full_end_time;
        if (i == packets.size() - 1)
            end_time = identification.time_of_units(ends[i] - trailing_trim_units);

        // Audio before the start of the stream is decoded but not played. Opus decoders drop the pre-skip themselves,
        // as they do for Matroska, so for Opus it is only left out of the frame's time.
        auto presentation_time = start_time;
        auto leading_discard = AK::Duration::zero();
        if (identification.type == TrackType::Audio && start_time < AK::Duration::zero()) {
            presentation_time = min(end_time, AK::Duration::zero());
            if (identification.codec_id != CodecID::Opus)
                leading_discard = presentation_time - start_time;
        }

        auto frame = CodedFrame(identification.codec_id, status.link_start_offset + presentation_time, status.link_start_offset + presentation_time, end_time - presentation_time, flags, move(packet), {});
        frame.set_leading_discard(leading_discard);
        frame.set_trailing_discard(full_end_time - end_time);
        status.ready_frames.enqueue(move(frame));
    }
    return {};
}

DecoderErrorOr<void> OggDemuxer::handle_page(TrackStatus& status, TrackDescription const& description, Page page)
{
    auto const& header = page.header;
    auto stream_index = status.link.index_of_serial_number(header.serial_number);
    if (!stream_index.has_value()) {
        if (!header.begins_stream())
            return {};
        // Keep header reads separate from playback so an interrupted link transition can be retried.
        PageIterator link_reader = status.pages;
        link_reader.set_position(header.position);
        auto link = read_link(link_reader);
        if (link.is_error()) {
            status.pages.set_position(header.position);
            return link.release_error();
        }
        auto start_offset = status.link_start_offset + status.link_end_time;
        TRY(enter_link(status, description, link.release_value(), start_offset));
        status.pages.set_position(header.position);
        return {};
    }

    auto const& stream = status.link.streams[*stream_index];
    if (stream.identification.has_value() && header.has_granule_position()) {
        auto end_time = stream.identification->time_of_units(stream.identification->end_units_of_granule_position(header.granule_position));
        status.link_end_time = max(status.link_end_time, end_time);
    }
    if (stream_index != status.stream_index)
        return {};

    auto packets = status.assembler.take_completed_packets(page);
    if (packets.is_error()) {
        if (packets.error().category() == DecoderErrorCategory::Corrupted)
            return {};
        return packets.release_error();
    }
    return queue_packets(status, header, packets.release_value());
}

DecoderErrorOr<CodedFrame> OggDemuxer::get_next_sample_for_track(Track const& track)
{
    auto const& description = track_description(track);
    auto& status = track_status(track);

    while (true) {
        if (!status.ready_frames.is_empty()) {
            auto frame = status.ready_frames.dequeue();
            if (status.needs_codec_configuration) {
                status.needs_codec_configuration = false;
                auto const& stream = status.link.streams[*status.stream_index];
                frame.set_new_codec_configuration(DECODER_TRY_ALLOC(stream.codec_initialization_data.clone()));
            }
            return frame;
        }
        if (!status.stream_index.has_value())
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "The track has no stream in this link"sv);
        auto page = TRY(status.pages.read_next_page());
        TRY(handle_page(status, description, move(page)));
    }
}

AK::Duration OggDemuxer::select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode)
{
    return target;
}

namespace {

struct TimedPage {
    size_t position { 0 };
    i64 end_units { 0 };
};

}

static DecoderErrorOr<Optional<TimedPage>> read_timed_page_at_or_after(PageIterator const& iterator, size_t position, size_t bound, u32 serial_number, StreamIdentification const& identification)
{
    PageIterator probe = iterator;
    probe.set_position(position);
    if (!TRY(probe.resync_forward(bound)))
        return Optional<TimedPage> {};
    while (probe.position() < bound) {
        auto result = TRY(probe.read_next_page_header());
        auto const* header = result.get_pointer<PageHeader>();
        if (header == nullptr)
            break;
        if (header->serial_number == serial_number && header->has_granule_position())
            return TimedPage { header->position, identification.end_units_of_granule_position(header->granule_position) };
    }
    return Optional<TimedPage> {};
}

DecoderErrorOr<OggDemuxer::LandingPage> OggDemuxer::find_landing_page(PageIterator const& iterator, size_t link_position, Optional<size_t> link_end_position, u32 serial_number, StreamIdentification const& identification, i64 target_units, Vector<PageAnchor> const& anchors) const
{
    if (target_units <= 0)
        return LandingPage { link_position, {} };

    struct Anchor {
        size_t byte { 0 };
        i64 units { 0 };
    };
    Anchor low { link_position, 0 };
    Optional<Anchor> high;
    auto consider = [&](PageAnchor const& anchor) {
        if (anchor.serial_number != serial_number || anchor.position < link_position)
            return;
        if (link_end_position.has_value() && anchor.position >= *link_end_position)
            return;
        if (anchor.end_units <= target_units) {
            if (anchor.end_units >= low.units)
                low = { anchor.position, anchor.end_units };
        } else if (!high.has_value() || anchor.end_units < high->units) {
            high = Anchor { anchor.position, anchor.end_units };
        }
    };
    for (auto const& anchor : anchors)
        consider(anchor);
    if (m_tail_anchor.has_value())
        consider(*m_tail_anchor);
    if (high.has_value() && high->byte <= low.byte)
        high = {};

    // Aim before the target, so that a probe lands before it rather than closing in from one side, and then walk.
    auto underestimate_target_by = max<i64>(identification.units_of_time(SEEK_LEAD), 1);
    for (size_t probe = 0; probe < MAXIMUM_SEEK_PROBE_COUNT && target_units - low.units > underestimate_target_by; probe++) {
        if (!high.has_value() || high->byte <= low.byte + 1 || high->units <= low.units)
            break;
        auto aim = target_units - underestimate_target_by;
        auto units_fraction = static_cast<double>(aim - low.units) / static_cast<double>(high->units - low.units);
        auto guess = low.byte + static_cast<size_t>(static_cast<double>(high->byte - low.byte) * units_fraction);
        guess = clamp(guess, low.byte + 1, high->byte - 1);

        auto page = TRY(read_timed_page_at_or_after(iterator, guess, high->byte, serial_number, identification));
        if (!page.has_value()) {
            // None of the stream's pages lie past the guess, so the one we know of is the closest.
            high->byte = guess;
            continue;
        }
        if (page->end_units <= target_units) {
            low = { page->position, page->end_units };
        } else {
            high = Anchor { page->position, page->end_units };
            underestimate_target_by *= 2;
        }
    }

    // Walk to the last of the stream's pages that ends by the target. The packet it ends with starts no later than that.
    LandingPage landing { low.byte, {} };
    PageIterator walker = iterator;
    walker.set_position(low.byte);
    auto passed_stream_starts = false;
    while (true) {
        if (link_end_position.has_value() && walker.position() >= *link_end_position)
            break;
        auto result = TRY(walker.read_next_page_header());
        auto const* header = result.get_pointer<PageHeader>();
        if (header == nullptr) {
            if (result.get<PageReadFailure>() == PageReadFailure::Incomplete)
                break;
            walker.set_position(walker.position() + 1);
            if (!TRY(walker.resync_forward()))
                break;
            continue;
        }
        if (header->begins_stream()) {
            if (passed_stream_starts)
                break;
            continue;
        }
        passed_stream_starts = true;
        if (header->serial_number != serial_number || !header->has_granule_position())
            continue;
        if (identification.end_units_of_granule_position(header->granule_position) > target_units)
            break;
        landing = { header->position, header->granule_position };
    }
    return landing;
}

DecoderErrorOr<DemuxerSeekResult> OggDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions options)
{
    auto const& description = track_description(track);
    auto& status = track_status(track);

    Vector<ScannedLink> links;
    Vector<PageAnchor> anchors;
    {
        MutexLocker locker { m_published_scan->mutex };
        links = m_published_scan->links;
        anchors = m_published_scan->anchors;
    }

    size_t link_position = m_first_link.position;
    auto start_offset = AK::Duration::zero();
    Optional<size_t> link_end_position;
    for (size_t i = 0; i < links.size(); i++) {
        auto const& link = links[i];
        if (!link.start_offset.has_value() || *link.start_offset > timestamp)
            continue;
        if (!link.index_of_stream_for_track(description.track.type(), description.index_within_type).has_value())
            continue;
        link_position = link.position;
        start_offset = *link.start_offset;
        link_end_position = {};
        if (i + 1 < links.size())
            link_end_position = links[i + 1].position;
    }

    Link link;
    if (link_position == status.link.position) {
        link = TRY(status.link.clone());
    } else if (link_position == m_first_link.position) {
        link = TRY(m_first_link.clone());
    } else {
        PageIterator link_reader = status.pages;
        link_reader.set_position(link_position);
        link = TRY(read_link(link_reader));
    }
    auto stream_index = link.index_of_stream_for_track(description.track.type(), description.index_within_type);
    if (!stream_index.has_value())
        return DecoderError::corrupted("The link we seek into does not carry the track"sv);
    auto const& stream = link.streams[*stream_index];
    auto const& identification = *stream.identification;

    auto local_target = timestamp - start_offset;
    if (identification.codec_id == CodecID::Opus)
        local_target -= OPUS_SEEK_PREROLL;
    auto target_units = identification.units_of_time(local_target);
    LandingPage landing;
    if (identification.codec_id == CodecID::Theora) {
        // Land on a page ending by the target frame, then on the page before the keyframe that frame depends on.
        landing = TRY(find_landing_page(status.pages, link.position, link_end_position, stream.serial_number, identification, target_units + 1, anchors));
        if (landing.granule_position.has_value()) {
            auto keyframe_units = identification.theora_keyframe_start_units(*landing.granule_position);
            landing = TRY(find_landing_page(status.pages, link.position, link_end_position, stream.serial_number, identification, keyframe_units, anchors));
        }
    } else {
        landing = TRY(find_landing_page(status.pages, link.position, link_end_position, stream.serial_number, identification, target_units, anchors));
    }

    if (link.position != status.link.position)
        TRY(enter_link(status, description, move(link), start_offset));
    status.pages.set_position(landing.position);
    status.assembler.reset();
    status.running_end_units = {};
    status.ready_frames.clear();
    status.packet_duration_parser.reset();
    status.waiting_for_keyframe = description.track.type() == TrackType::Video;
    status.reading_seek_preroll_page = landing.granule_position.has_value();
    if (has_flag(options, DemuxerSeekOptions::NeedCodecConfiguration))
        status.needs_codec_configuration = true;
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<AK::Duration> OggDemuxer::total_duration()
{
    return m_duration;
}

DecoderErrorOr<AK::Duration> OggDemuxer::duration_of_track(Track const&)
{
    return total_duration();
}

DemuxerScanState const& OggDemuxer::scan_state() const
{
    return m_buffered_scan_thread->main_thread_state();
}

void OggDemuxer::set_scan_state_change_handler(ScanStateChangeHandler handler)
{
    m_buffered_scan_thread->set_change_handler([handler = move(handler)] {
        if (handler)
            handler(TimeRanges {});
    });
}

void OggDemuxer::set_blocking_reads_aborted_for_track(Track const& track)
{
    track_status(track).pages.cursor().abort();
}

void OggDemuxer::reset_blocking_reads_aborted_for_track(Track const& track)
{
    track_status(track).pages.cursor().reset_abort();
}

void OggDemuxer::set_read_blocked_change_handler_for_track(Track const& track, ReadBlockedChangeHandler handler)
{
    track_status(track).pages.cursor().set_blocked_change_handler(move(handler));
}

}
