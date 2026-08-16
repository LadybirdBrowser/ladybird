/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextInputBox.h>

namespace Web::Layout {

TextInputBox::TextInputBox(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::LayoutStyle style)
    : BlockContainer(document, element, style)
{
}

}
