/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Containers/IndexEntry.h>

#include "ContainerNavigator.h"

namespace Media {

class IndexedContainerNavigator final : public ContainerNavigator {
public:
    struct TrackIndex {
        u64 track_identifier;
        Vector<IndexEntry> entries;
    };

    IndexedContainerNavigator(Vector<TrackIndex>&& track_indices, AK::Duration duration)
        : m_track_indices(move(track_indices))
        , m_duration(duration)
    {
    }

    virtual HashMap<u64, BufferedRangesScan> buffered_time_ranges_by_track(Vector<MediaStream::ByteRange> const& byte_ranges) const override;

private:
    static size_t lower_bound(Vector<IndexEntry> const& entries, size_t target);

    Vector<TrackIndex> m_track_indices;
    AK::Duration m_duration;
};

}
