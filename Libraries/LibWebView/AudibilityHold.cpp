/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/Audibility.h>
#include <LibWebView/AudibilityHold.h>

namespace WebView {

void AudibilityHold::page_began_hosting(Web::PageId page_id)
{
    m_left_pages.remove(page_id);
}

bool AudibilityHold::page_left(Web::PageId page_id, MonotonicTime now)
{
    if (m_left_pages.set(page_id) != HashSetResult::InsertedNewEntry)
        return false;
    auto held_until = now + Audio::AUDIBILITY_LINGER;
    if (m_held_until.has_value() && *m_held_until >= held_until)
        return false;
    m_held_until = held_until;
    return true;
}

void AudibilityHold::page_closed(Web::PageId page_id)
{
    m_left_pages.remove(page_id);
}

bool AudibilityHold::is_held(MonotonicTime now) const
{
    return m_held_until.has_value() && now < *m_held_until;
}

}
