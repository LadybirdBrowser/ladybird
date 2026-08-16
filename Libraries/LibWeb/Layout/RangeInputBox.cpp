/*
 * Copyright (c) 2026, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/RangeInputBox.h>

namespace Web::Layout {

RangeInputBox::RangeInputBox(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::LayoutStyle style)
    : BlockContainer(document, element, style)
{
}

}
