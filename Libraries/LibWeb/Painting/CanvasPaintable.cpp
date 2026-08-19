/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/CanvasPaintable.h>

namespace Web::Painting {

NonnullRefPtr<CanvasPaintable> CanvasPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new CanvasPaintable(layout_box));
}

CanvasPaintable::CanvasPaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

}
