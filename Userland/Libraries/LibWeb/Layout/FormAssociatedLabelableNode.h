/*
 * Copyright (c) 2022, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Layout/LabelableNode.h>

namespace Web::Layout {

class FormAssociatedLabelableNode : public LabelableNode {
    JS_CELL(FormAssociatedLabelableNode, LabelableNode);

public:
    const HTML::FormAssociatedElement& dom_node() const { return dynamic_cast<const HTML::FormAssociatedElement&>(LabelableNode::dom_node()); }
    HTML::FormAssociatedElement& dom_node() { return dynamic_cast<HTML::FormAssociatedElement&>(LabelableNode::dom_node()); }

protected:
    FormAssociatedLabelableNode(DOM::Document& document, HTML::FormAssociatedElement& element, CSS::StyleProperties style)
        : LabelableNode(document, element.form_associated_element_to_html_element(), move(style))
    {
    }

    virtual ~FormAssociatedLabelableNode() = default;
};

}
