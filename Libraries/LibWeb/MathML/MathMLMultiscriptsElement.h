/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLMultiscriptsElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLMultiscriptsElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLMultiscriptsElement);

public:
    virtual ~MathMLMultiscriptsElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLMultiscriptsElement(DOM::Document&, DOM::QualifiedName);
};

}
