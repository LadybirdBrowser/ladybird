/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLPaddedBox.h>
#include <LibWeb/MathML/MathMLPaddedElement.h>

namespace Web::Layout {

MathMLPaddedBox::MathMLPaddedBox(DOM::Document& document, MathML::MathMLPaddedElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
