/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLScriptBox.h>
#include <LibWeb/MathML/MathMLScriptElement.h>

namespace Web::Layout {

MathMLScriptBox::MathMLScriptBox(DOM::Document& document, MathML::MathMLScriptElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
