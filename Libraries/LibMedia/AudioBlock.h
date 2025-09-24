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
    AK::Duration start_timestamp() const { return m_start_timestamp; }
    Data& data() { return m_data; }
    Data const& data() const { return m_data; }

    void clear()
    {
        m_sample_rate = 0;
        m_channel_count = 0;
        m_start_timestamp = AK::Duration::zero();
        m_data = Data();
    }
    template<typename Callback>
    void emplace(u32 sample_rate, u8 channel_count, AK::Duration start_timestamp, Callback data_callback)
    {
        VERIFY(sample_rate != 0);
        VERIFY(channel_count != 0);
        VERIFY(m_data.is_empty());
        m_sample_rate = sample_rate;
        m_channel_count = channel_count;
        m_start_timestamp = start_timestamp;
        data_callback(m_data);
        VERIFY(!Checked<i64>::multiplication_would_overflow(m_data.size(), channel_count));
    }
    bool is_empty() const
    {
        return m_sample_rate == 0;
    }
    i64 data_count() const
    {
        return static_cast<i64>(data().size());
    }
    i64 sample_count() const
    {
        return data_count() / m_channel_count;
    }

private:
    u32 m_sample_rate { 0 };
    u8 m_channel_count { 0 };
    AK::Duration m_start_timestamp;
    Data m_data;
};

}
