/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

ChromeWidget::ChromeWidget(Paintable& paintable)
    : m_paintable(paintable)
{
}

RefPtr<Paintable> ChromeWidget::paintable() const
{
    return m_paintable.strong_ref();
}

void ChromeWidget::detach_from_paintable(Badge<Paintable>)
{
    m_paintable.clear();
    did_detach_from_paintable();
}

}
