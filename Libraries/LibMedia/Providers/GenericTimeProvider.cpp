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
    if (m_monotonic_time_on_resume.has_value())
        time += MonotonicTime::now() - m_monotonic_time_on_resume.value();
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

void GenericTimeProvider::set_time(AK::Duration time)
{
    if (m_monotonic_time_on_resume.has_value())
        m_monotonic_time_on_resume.emplace(MonotonicTime::now());

    m_media_time = time;
}

}
