/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLScriptElement.h>

namespace Web::Layout {

class MathMLScriptBox final : public MathMLBox {
    GC_CELL(MathMLScriptBox, MathMLBox);

public:
    MathMLScriptBox(DOM::Document&, MathML::MathMLScriptElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLScriptBox() override = default;

    MathML::MathMLScriptElement& dom_node() { return static_cast<MathML::MathMLScriptElement&>(MathMLBox::dom_node()); }
    MathML::MathMLScriptElement const& dom_node() const { return static_cast<MathML::MathMLScriptElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_script_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLScriptBox>() const { return is_mathml_script_box(); }

}
