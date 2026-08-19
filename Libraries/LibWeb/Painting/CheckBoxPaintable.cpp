/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/CheckBoxPaintable.h>
#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

NonnullRefPtr<CheckBoxPaintable>
CheckBoxPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new CheckBoxPaintable(layout_box));
}

CheckBoxPaintable::CheckBoxPaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

}
