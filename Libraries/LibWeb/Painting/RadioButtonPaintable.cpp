/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/RadioButtonPaintable.h>

namespace Web::Painting {

NonnullRefPtr<RadioButtonPaintable> RadioButtonPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new RadioButtonPaintable(layout_box));
}

RadioButtonPaintable::RadioButtonPaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

}
