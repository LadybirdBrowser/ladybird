/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/OwnPtr.h>
#include <AK/Queue.h>
#include <LibMedia/Codecs/AudioPacketDurationParser.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Containers/CachedByteRanges.h>
#include <LibMedia/Containers/Ogg/Reader.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerScanThread.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

namespace Media::Ogg {

class MEDIA_API OggDemuxer final : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<Demuxer>> from_stream(NonnullRefPtr<MediaStream> const&);
    static bool supports_container_mime_type(ContainerMimeType);
    static bool supports_codec_in_container(ContainerID, CodecID);

    ~OggDemuxer();

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;
    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;
    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;
    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;
    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;
    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(ScanStateChangeHandler) override;
    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) override;

private:
    struct StreamDescription {
        u32 serial_number { 0 };
        Optional<StreamIdentification> identification;
    };

    struct LinkStream : StreamDescription {
        PageHeader beginning_page;
        FixedArray<u8> codec_initialization_data;
    };

    struct StreamHeaderReader {
        LinkStream stream;
        PacketAssembler assembler;
        Vector<FixedArray<u8>> packets;
        bool complete { false };

        bool needs_more_pages() const { return stream.identification.has_value() && !complete; }
        DecoderErrorOr<void> consume(Page const&);
    };

    // A chained file contains successive links, each containing concurrent logical streams.
    struct Link {
        size_t position { 0 };
        Vector<LinkStream> streams;

        DecoderErrorOr<Link> clone() const;
        Optional<size_t> index_of_serial_number(u32) const;
        Optional<size_t> index_of_stream_for_track(TrackType, size_t index_within_type) const;
    };

    struct TrackDescription {
        Track track;
        size_t index_within_type { 0 };
    };

    struct ScannedLink {
        size_t position { 0 };
        Vector<StreamDescription> streams;
        Optional<u32> preceding_stream_serial_number;
        Optional<AK::Duration> end_time;
        Optional<AK::Duration> start_offset;

        Optional<size_t> index_of_stream_for_track(TrackType, size_t index_within_type) const;
    };

    struct PageAnchor {
        u32 serial_number { 0 };
        size_t position { 0 };
        i64 end_units { 0 };
    };

    struct PublishedScan : public AtomicRefCounted<PublishedScan> {
        Mutex mutex;
        Vector<ScannedLink> links;
        Vector<PageAnchor> anchors;
    };

    struct SerialExtent {
        PageHeader first_page;
        PageHeader last_page;
    };

    struct ScannedByteRange : public CachedByteRange {
        using CachedByteRange::CachedByteRange;

        Optional<size_t> next_page_position;
        bool needs_resync { true };
        Optional<PageHeader> previous_page;
        Vector<SerialExtent> extents;
    };

    struct ScannedStream {
        StreamDescription description;
        size_t beginning_page_position { 0 };
        Optional<PageHeader> preceding_page {};
        Optional<i64> maximum_granule_position {};
        Optional<StreamHeaderReader> pending_headers {};
        size_t next_header_page_position { 0 };
        Codecs::AudioPacketDurationParser packet_duration_parser {};
    };

    struct BufferedScanPayload {
        PageIterator walker;
        CachedByteRanges<ScannedByteRange> scanned_ranges;
        HashMap<u32, ScannedStream> streams;
        NonnullRefPtr<PublishedScan> published;

        DemuxerScanState scan_available_ranges(MediaStream::Availability const&, ReadonlySpan<TrackDescription>, size_t first_link_position, AK::Duration minimum_duration);
        void walk(ScannedByteRange&);
        void scan_page(ScannedByteRange&, PageHeader const&);
        void read_pending_headers(ScannedStream&) const;
        Vector<ScannedLink> assemble_links(size_t first_link_position) const;
        i64 start_units_of_page(PageHeader const&, ScannedStream const&, i64 end_units);
    };

    struct TrackStatus {
        AK_ALLOC_WITH_KMALLOC;

        PageIterator pages;
        PacketAssembler assembler {};
        Link link {};
        Optional<size_t> stream_index {};
        AK::Duration link_start_offset {};
        AK::Duration link_end_time {};
        Codecs::AudioPacketDurationParser packet_duration_parser {};
        Optional<i64> running_end_units {};
        bool needs_codec_configuration { true };
        bool waiting_for_keyframe { false };
        bool reading_seek_preroll_page { false };
        Queue<CodedFrame> ready_frames {};
    };

    struct LandingPage {
        size_t position { 0 };
        Optional<i64> granule_position;
    };

    OggDemuxer(NonnullRefPtr<MediaStream> const&, Link, Vector<TrackDescription>, AK::Duration duration, Optional<PageAnchor> tail_anchor);

    static DecoderErrorOr<Link> read_link(PageIterator&);

    void start_buffered_scan_thread();
    TrackStatus& track_status(Track const&);
    TrackDescription const& track_description(Track const&) const;
    static DecoderErrorOr<void> enter_link(TrackStatus&, TrackDescription const&, Link, AK::Duration start_offset);
    static DecoderErrorOr<void> handle_page(TrackStatus&, TrackDescription const&, Page);
    static DecoderErrorOr<void> queue_packets(TrackStatus&, PageHeader const&, Vector<FixedArray<u8>>);
    DecoderErrorOr<LandingPage> find_landing_page(PageIterator const&, size_t link_position, Optional<size_t> link_end_position, u32 serial_number, StreamIdentification const&, i64 target_units, Vector<PageAnchor> const&) const;

    NonnullRefPtr<MediaStream> m_stream;
    Link m_first_link;
    Vector<TrackDescription> m_tracks;
    AK::Duration m_duration;
    Optional<PageAnchor> m_tail_anchor;
    NonnullRefPtr<PublishedScan> m_published_scan { make_ref_counted<PublishedScan>() };
    RefPtr<DemuxerScanThread<BufferedScanPayload>> m_buffered_scan_thread;
    Mutex m_track_statuses_mutex;
    Vector<OwnPtr<TrackStatus>> m_track_statuses;
};

}
