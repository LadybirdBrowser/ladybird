/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Sample.h"
#include <AK/DisjointChunks.h>
#include <AK/FixedArray.h>
#include <AK/Noncopyable.h>
#include <AK/Vector.h>
#include <LibThreading/Mutex.h>

namespace Audio {

// A sample queue providing synchronized access to efficiently-stored segmented user-provided audio data.
class UserSampleQueue {
    AK_MAKE_NONCOPYABLE(UserSampleQueue);
    AK_MAKE_NONMOVABLE(UserSampleQueue);

public:
    UserSampleQueue() = default;

    void append(FixedArray<Sample>&& samples);
    void clear();
    // Slice off some amount of samples from the beginning.
    void discard_samples(size_t count);
    Sample operator[](size_t index);

    // The number of samples in the span.
    size_t size();
    bool is_empty();
    size_t remaining_samples();

private:
    // Re-initialize the spans after a vector resize.
    void fix_spans();

    Threading::Mutex m_sample_mutex;
    // Sample data view to keep track of what to play next.
    DisjointSpans<Sample> m_enqueued_samples;
    // The number of samples that were played from the backing store since last discarding its start.
    size_t m_samples_to_discard { 0 };
    // The backing store for the enqueued sample view.
    DisjointChunks<Sample, FixedArray<Sample>> m_backing_samples {};
};

}
