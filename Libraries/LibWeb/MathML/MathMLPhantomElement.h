/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLPhantomElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLPhantomElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLPhantomElement);

public:
    virtual ~MathMLPhantomElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLPhantomElement(DOM::Document&, DOM::QualifiedName);
};

}
