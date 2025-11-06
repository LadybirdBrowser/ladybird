/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLTableElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLTableElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLTableElement);

public:
    virtual ~MathMLTableElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLTableElement(DOM::Document&, DOM::QualifiedName);
};

}
