/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MonotonicMediaClock.h"

namespace Media {

ErrorOr<NonnullRefPtr<MonotonicMediaClock>> MonotonicMediaClock::try_create()
{
    auto time_writer = TRY(MediaTimeWriter::create());
    auto time_reader = TRY(MediaTimeReader::create(time_writer.buffer()));
    return adopt_nonnull_ref_or_enomem(new (nothrow) MonotonicMediaClock(move(time_writer), move(time_reader)));
}

MonotonicMediaClock::MonotonicMediaClock(MediaTimeWriter time_writer, MediaTimeReader time_reader)
    : m_time_writer(move(time_writer))
    , m_time_reader(move(time_reader))
{
}

MonotonicMediaClock::~MonotonicMediaClock() = default;

MediaTimeReader MonotonicMediaClock::time_reader() const
{
    return m_time_reader;
}

void MonotonicMediaClock::resume()
{
    auto now = MonotonicTime::now();
    auto time = m_time_reader.current_time(now);
    m_time_writer.publish_monotonic_anchor(now, time, m_playback_rate, true);
    m_playing = true;
}

void MonotonicMediaClock::pause()
{
    if (!m_playing)
        return;
    auto now = MonotonicTime::now();
    auto time = m_time_reader.current_time(now);
    m_time_writer.publish_monotonic_anchor(now, time, m_playback_rate, false);
    m_playing = false;
}

void MonotonicMediaClock::seek(AK::Duration time)
{
    m_time_writer.seek(time);
    if (m_playing)
        m_time_writer.publish_monotonic_anchor(MonotonicTime::now(), time, m_playback_rate, true);
}

void MonotonicMediaClock::set_playback_rate(float rate)
{
    VERIFY(rate >= 0);
    if (m_playback_rate == rate)
        return;
    m_playback_rate = rate;
    if (m_playing) {
        auto now = MonotonicTime::now();
        auto time = m_time_reader.current_time(now);
        m_time_writer.publish_monotonic_anchor(now, time, rate, true);
    }
}

}
