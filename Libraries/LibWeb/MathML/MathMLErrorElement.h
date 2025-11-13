/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLErrorElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLErrorElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLErrorElement);

public:
    virtual ~MathMLErrorElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLErrorElement(DOM::Document&, DOM::QualifiedName);
};

}
