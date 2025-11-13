/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLTableRowElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLTableRowElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLTableRowElement);

public:
    virtual ~MathMLTableRowElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLTableRowElement(DOM::Document&, DOM::QualifiedName);
};

}
