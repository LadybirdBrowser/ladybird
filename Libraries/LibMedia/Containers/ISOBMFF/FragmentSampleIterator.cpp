/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/SaturatingMath.h>

#include "FragmentSampleIterator.h"

namespace Media::ISOBMFF {

FragmentSampleIterator::FragmentSampleIterator(MovieFragment&& fragment)
    : m_fragment(move(fragment))
{
    enter_run();
}

Sample FragmentSampleIterator::peek() const
{
    VERIFY(has_next());
    auto const& run = m_fragment.track_runs[m_run_index];
    auto decode_time = saturating_add(m_decode_time, run.composition_to_presentation_offset);
    auto presentation_time = saturating_add(decode_time, run.composition_time_offset_of_sample(m_sample_index));
    auto presentation_end_time = saturating_add(presentation_time, static_cast<i64>(run.duration_of_sample(m_sample_index)));

    auto presentation_timestamp = AK::Duration::from_time_units(presentation_time, 1, run.timescale);
    auto presentation_end = AK::Duration::from_time_units(presentation_end_time, 1, run.timescale);

    return Sample {
        .track_id = run.track_id,
        .sample_description_index = run.defaults.sample_description_index,
        .decode_timestamp = AK::Duration::from_time_units(decode_time, 1, run.timescale),
        .presentation_timestamp = presentation_timestamp,
        .duration = presentation_end - presentation_timestamp,
        .is_sync_sample = (run.flags_of_sample(m_sample_index) & SAMPLE_IS_NON_SYNC_SAMPLE) == 0,
        .data_position = m_data_position,
        .data_size = run.size_of_sample(m_sample_index),
    };
}

void FragmentSampleIterator::advance()
{
    VERIFY(has_next());
    auto const& run = m_fragment.track_runs[m_run_index];
    m_decode_time = saturating_add(m_decode_time, static_cast<i64>(run.duration_of_sample(m_sample_index)));
    m_data_position += run.size_of_sample(m_sample_index);
    m_sample_index++;
    if (m_sample_index < run.sample_count)
        return;
    m_run_index++;
    m_sample_index = 0;
    enter_run();
}

void FragmentSampleIterator::enter_run()
{
    while (m_run_index < m_fragment.track_runs.size() && m_fragment.track_runs[m_run_index].sample_count == 0)
        m_run_index++;
    if (!has_next())
        return;
    m_decode_time = m_fragment.track_runs[m_run_index].first_decode_time;
    m_data_position = m_fragment.track_runs[m_run_index].data_position;
}

}
