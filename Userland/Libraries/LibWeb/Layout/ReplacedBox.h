/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class ReplacedBox : public Box {
    JS_CELL(ReplacedBox, Box);

public:
    ReplacedBox(DOM::Document&, DOM::Element&, NonnullRefPtr<CSS::StyleProperties>);
    virtual ~ReplacedBox() override;

    DOM::Element const& dom_node() const { return verify_cast<DOM::Element>(*Node::dom_node()); }
    DOM::Element& dom_node() { return verify_cast<DOM::Element>(*Node::dom_node()); }

    Optional<CSSPixels> intrinsic_width() const { return m_intrinsic_width; }
    Optional<CSSPixels> intrinsic_height() const { return m_intrinsic_height; }

    bool has_intrinsic_width() const { return intrinsic_width().has_value(); }
    bool has_intrinsic_height() const { return intrinsic_height().has_value(); }

    void set_intrinsic_width(Optional<CSSPixels> width) { m_intrinsic_width = width; }
    void set_intrinsic_height(Optional<CSSPixels> height) { m_intrinsic_height = height; }

    virtual void prepare_for_replaced_layout() { }

    virtual bool can_have_children() const override { return false; }

private:
    virtual bool is_replaced_box() const final { return true; }

    Optional<CSSPixels> m_intrinsic_width;
    Optional<CSSPixels> m_intrinsic_height;
    Optional<float> m_intrinsic_aspect_ratio;
};

template<>
inline bool Node::fast_is<ReplacedBox>() const { return is_replaced_box(); }

}
