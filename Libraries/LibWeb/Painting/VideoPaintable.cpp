/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Gregory Bertilso <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/VideoPaintable.h>

namespace Web::Painting {

NonnullRefPtr<VideoPaintable> VideoPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new VideoPaintable(layout_box));
}

VideoPaintable::VideoPaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

}
