/*
 * Copyright (c) 2025, Lorenz Ackermann, <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLMspaceElement final : public MathMLElement {
    WEB_NON_IDL_PLATFORM_OBJECT(MathMLMspaceElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLMspaceElement);

public:
    virtual ~MathMLMspaceElement() override = default;

private:
    MathMLMspaceElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
};

}
