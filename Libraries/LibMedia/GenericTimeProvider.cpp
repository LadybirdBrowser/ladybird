/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GenericTimeProvider.h"

namespace Media {

GenericTimeProvider::GenericTimeProvider() = default;

GenericTimeProvider::~GenericTimeProvider() = default;

AK::Duration GenericTimeProvider::current_time() const
{
    auto time = m_media_time;
    if (m_monotonic_time_on_resume.has_value()) {
        auto elapsed = MonotonicTime::now() - m_monotonic_time_on_resume.value();
        if (m_playback_rate != 1.0f)
            elapsed = AK::Duration::from_seconds_f64(elapsed.to_seconds_f64() * m_playback_rate);
        time += elapsed;
    }
    return time;
}

void GenericTimeProvider::resume()
{
    m_monotonic_time_on_resume.emplace(MonotonicTime::now());
}

void GenericTimeProvider::pause()
{
    if (!m_monotonic_time_on_resume.has_value())
        return;
    m_media_time = current_time();
    m_monotonic_time_on_resume = {};
}

void GenericTimeProvider::seek(AK::Duration time)
{
    if (m_monotonic_time_on_resume.has_value())
        m_monotonic_time_on_resume.emplace(MonotonicTime::now());

    m_media_time = time;
}

void GenericTimeProvider::set_playback_rate(float rate)
{
    VERIFY(rate >= 0);
    if (m_playback_rate == rate)
        return;
    if (m_monotonic_time_on_resume.has_value()) {
        m_media_time = current_time();
        m_monotonic_time_on_resume.emplace(MonotonicTime::now());
    }
    m_playback_rate = rate;
}

}
