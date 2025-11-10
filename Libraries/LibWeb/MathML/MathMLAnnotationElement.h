/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLAnnotationElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLAnnotationElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLAnnotationElement);

public:
    virtual ~MathMLAnnotationElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLAnnotationElement(DOM::Document&, DOM::QualifiedName);
};

}
