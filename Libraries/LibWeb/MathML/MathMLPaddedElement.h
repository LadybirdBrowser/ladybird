/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLPaddedElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLPaddedElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLPaddedElement);

public:
    virtual ~MathMLPaddedElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLPaddedElement(DOM::Document&, DOM::QualifiedName);
};

}
