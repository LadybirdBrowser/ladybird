/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML {

// Base class for msqrt (square root) and mroot (nth root) elements
class MathMLRadicalElement : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLRadicalElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLRadicalElement);

public:
    virtual ~MathMLRadicalElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    bool is_square_root() const { return local_name() == TagNames::msqrt; }

protected:
    MathMLRadicalElement(DOM::Document&, DOM::QualifiedName);
};

}
