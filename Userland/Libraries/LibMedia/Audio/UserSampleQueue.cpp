/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UserSampleQueue.h"

namespace Audio {

void UserSampleQueue::append(FixedArray<Sample>&& samples)
{
    Threading::MutexLocker lock(m_sample_mutex);
    if (m_samples_to_discard != 0)
        m_backing_samples = m_backing_samples.release_slice(m_samples_to_discard);
    m_backing_samples.append(move(samples));
    fix_spans();
}

void UserSampleQueue::clear()
{
    discard_samples(size());
}

void UserSampleQueue::fix_spans()
{
    Threading::MutexLocker lock(m_sample_mutex);
    m_enqueued_samples = m_backing_samples.spans();
    m_samples_to_discard = 0;
}

Sample UserSampleQueue::operator[](size_t index)
{
    Threading::MutexLocker lock(m_sample_mutex);
    return m_enqueued_samples[index];
}

void UserSampleQueue::discard_samples(size_t count)
{
    Threading::MutexLocker lock(m_sample_mutex);
    m_samples_to_discard += count;
    m_enqueued_samples = m_enqueued_samples.slice(count);
}

size_t UserSampleQueue::size()
{
    Threading::MutexLocker lock(m_sample_mutex);
    return m_enqueued_samples.size();
}

size_t UserSampleQueue::remaining_samples()
{
    Threading::MutexLocker lock(m_sample_mutex);
    VERIFY(m_backing_samples.size() >= m_samples_to_discard);
    return m_backing_samples.size() - m_samples_to_discard;
}

bool UserSampleQueue::is_empty()
{
    Threading::MutexLocker lock(m_sample_mutex);
    return m_enqueued_samples.is_empty();
}

}
