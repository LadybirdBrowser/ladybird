/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/LingeringIndicator.h>

namespace WebView {

void LingeringIndicator::set_active(bool active, MonotonicTime now)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active)
        m_visible_until.clear();
    else
        m_visible_until = now + m_linger;
}

bool LingeringIndicator::is_visible(MonotonicTime now) const
{
    return m_active || (m_visible_until.has_value() && now < *m_visible_until);
}

}
