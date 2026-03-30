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
    IndexedContainerNavigator(Vector<IndexEntry>&& entries, AK::Duration duration)
        : m_entries(move(entries))
        , m_duration(duration)
    {
    }

    TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const override;

private:
    size_t lower_bound(size_t target) const;

    Vector<IndexEntry> m_entries;
    AK::Duration m_duration;
};

}
