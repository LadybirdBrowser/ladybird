/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLStringElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLStringElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLStringElement);

public:
    virtual ~MathMLStringElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLStringElement(DOM::Document&, DOM::QualifiedName);
};

}
