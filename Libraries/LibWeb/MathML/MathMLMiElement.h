/*
 * Copyright (c) 2025, Lorenz Ackermann, <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLMiElement final : public MathMLElement {
    WEB_NON_IDL_WRAPPABLE(MathMLMiElement, MathMLElement);
    friend struct DOM::RustElementObjectHooks;

private:
    explicit MathMLMiElement(DOM::VtableProbe);
    virtual ~MathMLMiElement() override = default;

    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
