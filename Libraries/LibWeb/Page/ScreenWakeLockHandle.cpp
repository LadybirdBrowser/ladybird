/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/ScreenWakeLockHandle.h>

namespace Web {

ScreenWakeLockHandle::ScreenWakeLockHandle(Page& page)
    : m_page(page)
{
    m_page->acquire_screen_wake_lock();
}

ScreenWakeLockHandle::~ScreenWakeLockHandle()
{
    m_page->release_screen_wake_lock();
}

void ScreenWakeLockHandle::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_page);
}

}
