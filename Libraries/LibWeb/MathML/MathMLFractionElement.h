/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLFractionElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLFractionElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLFractionElement);

public:
    virtual ~MathMLFractionElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLFractionElement(DOM::Document&, DOM::QualifiedName);
};

}
