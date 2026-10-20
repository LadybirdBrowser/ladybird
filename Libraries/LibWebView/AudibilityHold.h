/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Export.h>

namespace WebView {

// Keeps a tab audible after an audible page leaves it, for as long as the page would have lingered had its sound stopped
// as it left. A page leaves once each time it begins hosting the tab's documents. Closing isn't leaving: a closed page
// holds nothing, and ends no hold.
class WEBVIEW_API AudibilityHold {
public:
    void page_began_hosting(Web::PageId);
    bool page_left(Web::PageId, MonotonicTime now);
    void page_closed(Web::PageId);

    bool is_held(MonotonicTime now) const;
    Optional<MonotonicTime> held_until() const { return m_held_until; }

private:
    HashTable<Web::PageId> m_left_pages;
    Optional<MonotonicTime> m_held_until;
};

}
