/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/CanvasBox.h>
#include <LibWeb/Painting/CanvasPaintable.h>

namespace Web::Layout {

CanvasBox::CanvasBox(DOM::Document& document, HTML::HTMLCanvasElement& element, CSS::LayoutStyle style)
    : ReplacedBox(document, element, style)
{
}

CanvasBox::~CanvasBox() = default;

}
