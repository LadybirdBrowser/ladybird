/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLSemanticsElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLSemanticsElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLSemanticsElement);

public:
    virtual ~MathMLSemanticsElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLSemanticsElement(DOM::Document&, DOM::QualifiedName);
};

}
