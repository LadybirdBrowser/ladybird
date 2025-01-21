/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class ReplacedBox : public Box {
    GC_CELL(ReplacedBox, Box);

public:
    ReplacedBox(DOM::Document&, DOM::Element&, GC::Ref<CSS::ComputedProperties>);
    virtual ~ReplacedBox() override;

    DOM::Element const& dom_node() const { return as<DOM::Element>(*Node::dom_node()); }
    DOM::Element& dom_node() { return as<DOM::Element>(*Node::dom_node()); }

    virtual void prepare_for_replaced_layout() { }

    virtual bool can_have_children() const override { return false; }

private:
    virtual bool is_replaced_box() const final { return true; }
};

template<>
inline bool Node::fast_is<ReplacedBox>() const { return is_replaced_box(); }

}
