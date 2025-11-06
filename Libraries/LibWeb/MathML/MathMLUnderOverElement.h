/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

// Base class for munder (underscript), mover (overscript), and munderover (both) elements
class MathMLUnderOverElement : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLUnderOverElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLUnderOverElement);

public:
    virtual ~MathMLUnderOverElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    enum class UnderOverType {
        Under,          // munder
        Over,           // mover
        UnderOver       // munderover
    };

    UnderOverType underover_type() const;

protected:
    MathMLUnderOverElement(DOM::Document&, DOM::QualifiedName);
};

}
