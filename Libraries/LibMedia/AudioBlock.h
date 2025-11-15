/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Time.h>

namespace Media {

class AudioBlock {
public:
    using Data = FixedArray<float>;

    u32 sample_rate() const { return m_sample_rate; }
    u8 channel_count() const { return m_channel_count; }
    AK::Duration timestamp() const { return m_timestamp; }
    i64 timestamp_in_samples() const { return m_timestamp_in_samples; }
    Data& data() { return m_data; }
    Data const& data() const { return m_data; }

    void clear()
    {
        m_sample_rate = 0;
        m_channel_count = 0;
        m_timestamp_in_samples = 0;
        m_data = Data();
    }
    template<typename Callback>
    void emplace(u32 sample_rate, u8 channel_count, AK::Duration timestamp, Callback data_callback)
    {
        VERIFY(sample_rate != 0);
        VERIFY(channel_count != 0);
        VERIFY(m_data.is_empty());
        m_sample_rate = sample_rate;
        m_channel_count = channel_count;
        m_timestamp = timestamp;
        m_timestamp_in_samples = timestamp.to_time_units(1, sample_rate);
        data_callback(m_data);
    }
    void set_timestamp_in_samples(i64 timestamp_in_samples)
    {
        VERIFY(!is_empty());
        m_timestamp_in_samples = timestamp_in_samples;
        m_timestamp = AK::Duration::from_time_units(timestamp_in_samples, 1, m_sample_rate);
    }
    bool is_empty() const
    {
        return m_sample_rate == 0;
    }
    size_t data_count() const
    {
        return data().size();
    }
    size_t sample_count() const
    {
        return data_count() / m_channel_count;
    }

private:
    u32 m_sample_rate { 0 };
    u8 m_channel_count { 0 };
    AK::Duration m_timestamp;
    i64 m_timestamp_in_samples { 0 };
    Data m_data;
};

}
