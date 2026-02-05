/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Math.h>
#include <AK/Time.h>
#include <LibMedia/Audio/SampleSpecification.h>

namespace Media {

class AudioBlock {
public:
    using Data = FixedArray<float>;

    Audio::SampleSpecification const& sample_specification() const { return m_sample_specification; }
    AK::Duration timestamp() const { return m_timestamp; }
    i64 timestamp_in_samples() const { return m_timestamp_in_samples; }
    i64 end_timestamp_in_samples() const { return Checked<i64>::saturating_add(m_timestamp_in_samples, AK::clamp_to<i64>(sample_count())); }
    AK::Duration end_timestamp() const { return AK::Duration::from_time_units(end_timestamp_in_samples(), 1, sample_rate()); }
    Data& data() { return m_data; }
    Data const& data() const { return m_data; }

    void clear()
    {
        m_sample_specification = {};
        m_timestamp_in_samples = 0;
        m_data = Data();
    }
    template<typename Callback>
    void emplace(Audio::SampleSpecification sample_specification, AK::Duration timestamp, Callback data_callback)
    {
        VERIFY(sample_specification.is_valid());
        m_sample_specification = sample_specification;
        m_timestamp = timestamp;
        m_timestamp_in_samples = timestamp.to_time_units(1, sample_rate());
        data_callback(m_data);
    }
    u32 sample_rate() const
    {
        return sample_specification().sample_rate();
    }
    void set_timestamp_in_samples(i64 timestamp_in_samples)
    {
        VERIFY(!is_empty());
        m_timestamp_in_samples = timestamp_in_samples;
        m_timestamp = AK::Duration::from_time_units(timestamp_in_samples, 1, sample_rate());
    }
    bool is_empty() const
    {
        return !sample_specification().is_valid();
    }
    size_t data_count() const
    {
        return data().size();
    }
    u8 channel_count() const
    {
        return sample_specification().channel_map().channel_count();
    }
    size_t sample_count() const
    {
        return data_count() / channel_count();
    }

private:
    Audio::SampleSpecification m_sample_specification;
    AK::Duration m_timestamp;
    i64 m_timestamp_in_samples { 0 };
    Data m_data;
};

}
