/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/Painting/ExternalContentSource.h>

namespace Web::Painting {

NonnullRefPtr<ExternalContentSource> ExternalContentSource::create()
{
    return adopt_ref(*new ExternalContentSource());
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
