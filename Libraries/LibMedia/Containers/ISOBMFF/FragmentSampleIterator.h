/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/Export.h>

#include "Boxes.h"

namespace Media::ISOBMFF {

struct Sample {
    u32 track_id { 0 };
    u32 sample_description_index { 0 };
    AK::Duration decode_timestamp;
    AK::Duration presentation_timestamp;
    AK::Duration duration;
    bool is_sync_sample { false };
    size_t data_position { 0 };
    size_t data_size { 0 };
};

class MEDIA_API FragmentSampleIterator {
public:
    explicit FragmentSampleIterator(MovieFragment&&);

    bool has_next() const { return m_run_index < m_fragment.track_runs.size(); }
    Sample peek() const;
    void advance();

private:
    void enter_run();

    MovieFragment m_fragment;
    size_t m_run_index { 0 };
    u32 m_sample_index { 0 };
    i64 m_decode_time { 0 };
    size_t m_data_position { 0 };
};

}
