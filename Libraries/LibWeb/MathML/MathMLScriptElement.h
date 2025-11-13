/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

// Base class for msub (subscript), msup (superscript), and msubsup (both) elements
class MathMLScriptElement : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLScriptElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLScriptElement);

public:
    virtual ~MathMLScriptElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    enum class ScriptType {
        Subscript,      // msub
        Superscript,    // msup
        SubSuperscript  // msubsup
    };

    ScriptType script_type() const;

protected:
    MathMLScriptElement(DOM::Document&, DOM::QualifiedName);
};

}
