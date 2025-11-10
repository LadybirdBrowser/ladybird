/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLSemanticsBox.h>
#include <LibWeb/MathML/MathMLSemanticsElement.h>

namespace Web::Layout {

MathMLSemanticsBox::MathMLSemanticsBox(DOM::Document& document, MathML::MathMLSemanticsElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
