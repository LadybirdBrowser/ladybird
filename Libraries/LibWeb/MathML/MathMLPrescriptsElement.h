/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLPrescriptsElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLPrescriptsElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLPrescriptsElement);

public:
    virtual ~MathMLPrescriptsElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

private:
    MathMLPrescriptsElement(DOM::Document&, DOM::QualifiedName);
};

}
