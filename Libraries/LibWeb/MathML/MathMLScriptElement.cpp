/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLScriptBox.h>
#include <LibWeb/MathML/MathMLScriptElement.h>
#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLScriptElement);

MathMLScriptElement::MathMLScriptElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLScriptElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLScriptBox>(document(), *this, move(style));
}

MathMLScriptElement::ScriptType MathMLScriptElement::script_type() const
{
    auto const& tag = local_name();
    if (tag == TagNames::msub)
        return ScriptType::Subscript;
    if (tag == TagNames::msup)
        return ScriptType::Superscript;
    if (tag == TagNames::msubsup)
        return ScriptType::SubSuperscript;

    // Default to subscript
    return ScriptType::Subscript;
}

}
