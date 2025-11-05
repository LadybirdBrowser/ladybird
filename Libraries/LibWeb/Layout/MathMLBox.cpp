/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLBox.h>

namespace Web::Layout {

MathMLBox::MathMLBox(DOM::Document& document, MathML::MathMLElement& element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, &element, move(style))
{
}

}
