/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>

namespace Web::Painting {

NonnullRefPtr<NavigableContainerViewportPaintable> NavigableContainerViewportPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new NavigableContainerViewportPaintable(layout_box));
}

NavigableContainerViewportPaintable::NavigableContainerViewportPaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

}
