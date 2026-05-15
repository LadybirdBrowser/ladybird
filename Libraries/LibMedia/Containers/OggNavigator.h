/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Variant.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Codecs/Vorbis.h>

#include "ScanningContainerNavigator.h"

namespace Media {

struct OggCachedRange : CachedByteRange {
    using CachedByteRange::CachedByteRange;

    Optional<AK::Duration> time_start;
    Optional<AK::Duration> time_end;
};

class OggNavigator final : public ScanningContainerNavigator<OggNavigator, OggCachedRange> {
public:
    static OwnPtr<OggNavigator> create(ReadonlyBytes first_packet, NonnullRefPtr<MediaStreamCursor>, CodecID, u32 time_base_numerator, u32 time_base_denominator, u32 sample_rate, ReadonlyBytes codec_initialization_data);

    void on_cached_range_changed(OggCachedRange& cached_range, CachedRangeChange change) const;
    static void append_time_range(OggCachedRange const& cached_range, TimeRanges& to);

private:
    struct Opus {
        u32 time_base_numerator { 1 };
        u32 time_base_denominator { 1 };
    };

    struct FLAC {
        u16 sync_code { 0 };
        u16 fixed_block_size { 0 };
        u32 sample_rate { 1 };
    };

    struct Vorbis {
        Codecs::Vorbis::Parser parser;
        u32 sample_rate { 1 };
    };

    using CodecData = Variant<Opus, FLAC, Vorbis>;

    struct PageScanResult {
        Optional<i64> granule_position;
        u64 parsed_packet_duration_in_granules;
        size_t byte_start;
        size_t byte_end;
    };

    OggNavigator(NonnullRefPtr<MediaStreamCursor>, CodecData);

    Optional<AK::Duration> find_start_timestamp(size_t search_start, size_t search_end) const;
    Optional<PageScanResult> find_last_page_with_valid_granule_position(size_t search_start, size_t search_end) const;
    Optional<AK::Duration> find_end_timestamp(size_t search_start, size_t search_end) const;
    Optional<PageScanResult> parse_page_at(size_t page_start, size_t search_end, ByteBuffer* continued_packet = nullptr) const;
    Optional<u64> read_packet_duration_in_granules(ReadonlyBytes) const;
    void reset_packet_parser_state() const;

    AK::Duration granule_to_time(i64 granule_position) const;

    NonnullRefPtr<MediaStreamCursor> m_cursor;
    CodecData m_codec_data;
    mutable ByteBuffer m_page_buffer;
};

}
