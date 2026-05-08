/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/Painting/ExternalContentSource.h>

namespace Web::Painting {

static Atomic<u64> s_next_id { 1 };

NonnullRefPtr<ExternalContentSource> ExternalContentSource::create()
{
    return adopt_ref(*new ExternalContentSource());
}

ExternalContentSource::ExternalContentSource()
    : m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

void ExternalContentSource::update(Optional<Gfx::DecodedImageFrame> frame)
{
    Optional<Gfx::DecodedImageFrame> old;
    {
        Threading::MutexLocker const locker { m_mutex };
        old = move(m_frame);
        m_frame = move(frame);
    }
}

void ExternalContentSource::clear()
{
    Optional<Gfx::DecodedImageFrame> old;
    {
        Threading::MutexLocker const locker { m_mutex };
        old = move(m_frame);
    }
}

Optional<Gfx::DecodedImageFrame> ExternalContentSource::current_frame() const
{
    Threading::MutexLocker const locker { m_mutex };
    return m_frame;
}

}
