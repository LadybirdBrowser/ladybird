/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLStyleBox.h>
#include <LibWeb/MathML/MathMLStyleElement.h>

namespace Web::Layout {

MathMLStyleBox::MathMLStyleBox(DOM::Document& document, MathML::MathMLStyleElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
