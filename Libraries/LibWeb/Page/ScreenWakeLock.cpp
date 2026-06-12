/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/ScreenWakeLock.h>

namespace Web {

ScreenWakeLock::ScreenWakeLock(Page& page)
    : m_page(page)
{
    m_page->acquire_screen_wake_lock();
}

ScreenWakeLock::~ScreenWakeLock()
{
    m_page->release_screen_wake_lock();
}

void ScreenWakeLock::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_page);
}

}
