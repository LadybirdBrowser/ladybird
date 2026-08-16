/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextAreaBox.h>

namespace Web::Layout {

TextAreaBox::TextAreaBox(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::LayoutStyle style)
    : BlockContainer(document, element, style)
{
}

}
